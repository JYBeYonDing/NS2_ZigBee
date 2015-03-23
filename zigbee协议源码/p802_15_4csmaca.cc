/********************************************/
/*     NS2 Simulator for IEEE 802.15.4      */
/*           (per P802.15.4/D18)            */
/*------------------------------------------*/
/* by:        Jianliang Zheng               */
/*        (zheng@ee.ccny.cuny.edu)          */
/*              Myung J. Lee                */
/*          (lee@ccny.cuny.edu)             */
/*        ~~~~~~~~~~~~~~~~~~~~~~~~~         */
/*           SAIT-CUNY Joint Lab            */
/********************************************/

// File:  p802_15_4csmaca.cc
// Mode:  C++; c-basic-offset:8; tab-width:8; indent-tabs-mode:t

// $Header: p802_15_4csmaca.cc,v 1.1 2004/10/15 17:32:08 zheng Exp $

/*
 * Copyright (c) 2003-2004 Samsung Advanced Institute of Technology and
 * The City University of New York. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the Joint Lab of Samsung 
 *      Advanced Institute of Technology and The City University of New York.
 * 4. Neither the name of Samsung Advanced Institute of Technology nor of 
 *    The City University of New York may be used to endorse or promote 
 *    products derived from this software without specific prior written 
 *    permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE JOINT LAB OF SAMSUNG ADVANCED INSTITUTE
 * OF TECHNOLOGY AND THE CITY UNIVERSITY OF NEW YORK ``AS IS'' AND ANY EXPRESS 
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES 
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN 
 * NO EVENT SHALL SAMSUNG ADVANCED INSTITUTE OR THE CITY UNIVERSITY OF NEW YORK 
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR 
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE 
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) 
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT 
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT 
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


#include "p802_15_4csmaca.h"
#include "p802_15_4const.h"
#include "p802_15_4trace.h"

//构造函数
CsmaCA802_15_4::CsmaCA802_15_4(Phy802_15_4 *p, Mac802_15_4 *m)
{
	phy = p;
	mac = m;
	//txPkt是一个包，这个包里什么都没有。。。。
	txPkt = 0;
	waitNextBeacon = false;
	//添加退避时间
	//退避时间更改函数需要更改macBackoffTimer()
	backoffT = new macBackoffTimer(this);
	//应用backoffT
	assert(backoffT);
	bcnOtherT = new macBeaconOtherTimer(this);
	assert(bcnOtherT);
	deferCCAT = new macDeferCCATimer(this);
	assert(deferCCAT);
}

//析构函数
CsmaCA802_15_4::~CsmaCA802_15_4()
{
	delete backoffT;
	delete bcnOtherT;
	delete deferCCAT;
}

//重置函数
//分为信标使能和非信标使能两部分
void CsmaCA802_15_4::reset(void)
{
	if (beaconEnabled)
	{
		NB = 0;
		CW = 2;
		BE = mac->mpib.macMinBE;
		if ((mac->mpib.macBattLifeExt)&&(BE > 2))
			BE = 2;
	}
	else
	{
		NB = 0;
		BE = mac->mpib.macMinBE;
	}
}

//获得竞争接入期的起始时间
double CsmaCA802_15_4::adjustTime(double wtime)
{
	//find the beginning point of CAP and adjust the scheduled time
	//if it comes before CAP
	double neg;
	double tmpf;
	//assert到底是啥意思？应用？
	assert(txPkt);
	//如果这个包有始发位置
	if (!mac->toParent(txPkt))
	{
		if (mac->mpib.macBeaconOrder != 15)
		{
			/* Linux floating number compatibility
			neg = (CURRENT_TIME + wtime - bcnTxTime) - mac->beaconPeriods * bPeriod;
			*/
			{
			tmpf = mac->beaconPeriods * bPeriod;
			tmpf = CURRENT_TIME - tmpf;
			tmpf += wtime;
			neg = tmpf - bcnTxTime;
			}

			if (neg < 0.0)
				wtime -= neg;
			return wtime;
		}
		else
			return wtime;
	}
	else
	{
		if (mac->macBeaconOrder2 != 15)
		{
			/* Linux floating number compatibility
			neg = (CURRENT_TIME + wtime - bcnRxTime) - mac->beaconPeriods2 * bPeriod;
			*/
			{
			tmpf = mac->beaconPeriods2 * bPeriod;
			tmpf = CURRENT_TIME - tmpf;
			tmpf += wtime;
			neg = tmpf - bcnRxTime;
			}

			if (neg < 0.0)
				wtime -= neg;
			return wtime;
		}
		else
			return wtime;
	}
}

bool CsmaCA802_15_4::canProceed(double wtime, bool afterCCA)
{
	//check if can proceed within the current superframe
	//(in the case the node acts as both a coordinator and a device, both the superframes from and to this node should be taken into account)
	hdr_cmn *ch = HDR_CMN(txPkt);	//for debug
	bool ok;
	UINT_16 t_bPeriods,t_CAP;
	double t_fCAP,t_CCATime,t_IFS,t_transacTime,bcnOtherTime,BI2;

	waitNextBeacon = false;
	wtime = mac->locateBoundary(mac->toParent(txPkt),wtime);
	if (!mac->toParent(txPkt))//该包是向父节点发送的
	{
		if (mac->mpib.macBeaconOrder != 15)
		{//如果是信标模式
			if (mac->sfSpec.BLE)
				//获得竞争接入时间
				t_CAP = mac->getBattLifeExtSlotNum();
			else
				//t_CAP = (超帧频谱的最后一帧） * （sfSpec.sd /单位退避时间） - 信标时间
				t_CAP = (mac->sfSpec.FinCAP + 1) * (mac->sfSpec.sd / aUnitBackoffPeriod) - mac->beaconPeriods;	//(mac->sfSpec.sd % aUnitBackoffPeriod) = 0
				
			/* Linux floating number compatibility
			t_bPeriods = (UINT_16)(((CURRENT_TIME + wtime - bcnTxTime) / bPeriod) - mac->beaconPeriods);
			*/
			{
			
			double tmpf;
			tmpf = CURRENT_TIME + wtime;
			tmpf -= bcnTxTime;
			tmpf /= bPeriod;
			t_bPeriods = (UINT_16)(tmpf - mac->beaconPeriods);
			}

			/* Linux floating number compatibility
			if (fmod(CURRENT_TIME + wtime - bcnTxTime, bPeriod) > 0.0)
			*/
			double tmpf;
			tmpf = CURRENT_TIME + wtime;
			tmpf -= bcnTxTime;
			if (fmod(tmpf, bPeriod) > 0.0)
				t_bPeriods++;
			bPeriodsLeft = t_bPeriods - t_CAP;
		}
		else
			bPeriodsLeft = -1;
	}
	else//该包不是向父节点发送的
	{
		if (mac->macBeaconOrder2 != 15)
		{
			//BI(两个连续信标间的持续时间，时间间隔）
			BI2 = mac->sfSpec2.BI / phy->getRate('s');
			
			/* Linux floating number compatibility
			t_CAP = (UINT_16)((mac->macBcnRxTime + (mac->sfSpec2.FinCAP + 1) * mac->sfSpec2.sd ) / phy->getRate('s'));
			*/
			{
			double tmpf;
			tmpf = (mac->sfSpec2.FinCAP + 1) * mac->sfSpec2.sd;
			tmpf += mac->macBcnRxTime;
			t_CAP = (UINT_16)(tmpf / phy->getRate('s'));
			}

			/* Linux floating number compatibility
			if (t_CAP + aMaxLostBeacons * BI2 < CURRENT_TIME)
			*/
			double tmpf;
			tmpf = aMaxLostBeacons * BI2;
			//t_CAP + tmpf 为该帧的总时间吗？
			if (t_CAP + tmpf < CURRENT_TIME)	
				bPeriodsLeft = -1;
			else
			{
				if (mac->sfSpec2.BLE)
					t_CAP = mac->getBattLifeExtSlotNum();
				else
					t_CAP = (mac->sfSpec2.FinCAP + 1) * (mac->sfSpec2.sd / aUnitBackoffPeriod) - mac->beaconPeriods2;	

				/* Linux floating number compatibility
				t_bPeriods = (UINT_16)(((CURRENT_TIME + wtime - bcnRxTime) / bPeriod) - mac->beaconPeriods2);
				*/
				{
				double tmpf;
				tmpf = CURRENT_TIME + wtime;
				tmpf -= bcnRxTime;
				tmpf /= bPeriod;
				t_bPeriods = (UINT_16)(tmpf - mac->beaconPeriods2);
				}

				/* Linux floating number compatibility
				if (fmod(CURRENT_TIME + wtime - bcnRxTime, bPeriod) > 0.0)
				*/
				double tmpf;
				tmpf = CURRENT_TIME + wtime;
				tmpf -= bcnRxTime;
				if (fmod(tmpf, bPeriod) > 0.0)
					t_bPeriods++;
				bPeriodsLeft = t_bPeriods - t_CAP;
			}
		}
		else
			bPeriodsLeft = -1;
	}

	ok = true;
	if (bPeriodsLeft > 0)
		ok = false;
	else if (bPeriodsLeft == 0)
	{
		if ((!mac->toParent(txPkt))
		&&  (!mac->sfSpec.BLE))
			ok = false;
		else if ((mac->toParent(txPkt))
		&&  (!mac->sfSpec2.BLE))
			ok = false;
	}
	if (!ok)
	{//如果没有足够的间隙
#ifdef DEBUG802_15_4
		fprintf(stdout,"[%s::%s][%f](node %d) cannot proceed: bPeriodsLeft = %d, orders = %d/%d/%d, type = %s, src = %d, dst = %d, uid = %d, mac_uid = %ld, size = %d\n",__FILE__,__FUNCTION__,CURRENT_TIME,mac->index_,bPeriodsLeft,mac->mpib.macBeaconOrder,mac->macBeaconOrder2,mac->macBeaconOrder3,wpan_pName(txPkt),p802_15_4macSA(txPkt),p802_15_4macDA(txPkt),ch->uid(),HDR_LRWPAN(txPkt)->uid,ch->size());
#endif
		if (mac->macBeaconOrder2 != 15)
		if (!mac->bcnRxT->busy())
			mac->bcnRxT->start();
		waitNextBeacon = true;
		return false;
	}

	//calculate the time needed to finish the transaction
	t_CCATime = 8 / phy->getRate('s');
	//从一个设备向另一个发送数据时，发送设备必须在两个连续发送的帧间进行简短的等待，
	//以允许接收设备在下一帧到达前对接收到的帧进行处理，这被称作帧间间隔（IFS）。
	//IFS的长度取决于发送帧的大小。小于或等于aMaxSIFSFramesSize的MPDU被当作短帧，
	//而长帧则是长度大于  aMaxSIFSFramesSize个字节的MPDU。
	if (HDR_CMN(txPkt)->size() <= aMaxSIFSFrameSize)
		t_IFS = aMinSIFSPeriod;
		//选择短帧间间隔
	else
		t_IFS = aMinLIFSPeriod;
		//选择长帧间间隔
		//macMinSIFSPeriod和macMinLIFSPeriod的值分别是12和40 symbols
	t_IFS /= phy->getRate('s');
	//获得标准时间单位？
	t_transacTime  = mac->locateBoundary(mac->toParent(txPkt),wtime) - wtime;
	//boundary location time -- should be 0 here, since we have already located the boundary
	if (!afterCCA)//在CSMA-CA中，当设备想要发送信息时，它将执行一条CCA来确保信道没有被其他设备使用，然后设备开始发送自己的信号。
	//所以在这里，是没有发送CCA的意思吗？
	{
		t_transacTime += t_CCATime;									//first CCA time
		t_transacTime += mac->locateBoundary(mac->toParent(txPkt),t_transacTime) - (t_transacTime);	//boundary location time for second CCA
		//难道说第一的CCA发出后，需要等待一个退避时隙后再反馈CCA
		t_transacTime += t_CCATime;									//second CCA time
	}
	t_transacTime += mac->locateBoundary(mac->toParent(txPkt),t_transacTime) - (t_transacTime);		//boundary location time for transmission
	t_transacTime += phy->trxTime(txPkt);									//packet transmission time
	if (ackReq)
	//如果需要应答帧，那么需要再把应答时间添上去
	{
		t_transacTime += mac->mpib.macAckWaitDuration/phy->getRate('s');
		//ack. waiting time (this value does not include round trip propagation delay)
		t_transacTime += 2*max_pDelay;
		//round trip propagation delay (802.15.4 ignores this, but it should be there even though it is very small)
		t_transacTime += t_IFS;
		//IFS time -- not only ensure that the sender can finish the transaction, but also the receiver
		t_fCAP = mac->getCAP(true);

		/* Linux floating number compatibility
		if (CURRENT_TIME + wtime + t_transacTime > t_fCAP)
		*/
		double tmpf;
		tmpf = CURRENT_TIME + wtime;
		tmpf += t_transacTime;
		if (tmpf > t_fCAP)
			ok = false;
		else
			ok= true;
	}
	else
	{
		//in this case, we need to handle individual CAP 
		ok = true;
		t_fCAP = mac->getCAPbyType(1);

		/* Linux floating number compatibility
		if (CURRENT_TIME + wtime + t_transacTime > t_fCAP)
		*/
		double tmpf;
		tmpf = CURRENT_TIME + wtime;
		tmpf += t_transacTime;
		if (tmpf > t_fCAP)
			ok = false;
		if (ok)
		{
			t_fCAP = mac->getCAPbyType(2);
			t_transacTime += max_pDelay;						//one-way trip propagation delay (802.15.4 ignores this, but it should be there even though it is very small)
			t_transacTime += 12/phy->getRate('s');					//transceiver turn-around time (receiver may need to do this to transmit next beacon)
			t_transacTime += t_IFS;							//IFS time -- not only ensure that the sender can finish the transaction, but also the receiver

			/* Linux floating number compatibility
			if (CURRENT_TIME + wtime + t_transacTime > t_fCAP)
			*/
			double tmpf;
			tmpf = CURRENT_TIME + wtime;
			tmpf += t_transacTime;
			if (tmpf > t_fCAP)
				ok = false;
		}
		if (ok)
		{
			t_fCAP = mac->getCAPbyType(3);
			t_transacTime -= t_IFS;							//the third node does not need to handle the transaction

			/* Linux floating number compatibility
			if (CURRENT_TIME + wtime + t_transacTime > t_fCAP)
			*/
			double tmpf;
			tmpf = CURRENT_TIME + wtime;
			tmpf += t_transacTime;
			if (tmpf > t_fCAP)
				ok = false;
		}
	}

	//check if have enough CAP to finish the transaction
	if (!ok)
	{
		bPeriodsLeft = 0;
		if ((mac->mpib.macBeaconOrder == 15)
		&&  (mac->macBeaconOrder2 == 15)
		&&  (mac->macBeaconOrder3 != 15))
		{
			/* Linux floating number compatibility
			bcnOtherTime = (mac->macBcnOtherRxTime + mac->sfSpec3.BI) / phy->getRate('s');
			*/
			{
			double tmpf;
			tmpf = (mac->macBcnOtherRxTime + mac->sfSpec3.BI);
			bcnOtherTime = tmpf / phy->getRate('s');
			}

			while (bcnOtherTime < CURRENT_TIME)
				bcnOtherTime += (mac->sfSpec3.BI / phy->getRate('s'));
			bcnOtherT->start(bcnOtherTime - CURRENT_TIME);
		}
#ifdef DEBUG802_15_4
	fprintf(stdout,"[%s::%s][%f](node %d) cannot proceed: orders = %d/%d/%d, type = %s, src = %d, dst = %d, uid = %d, mac_uid = %ld, size = %d\n",__FILE__,__FUNCTION__,CURRENT_TIME,mac->index_,mac->mpib.macBeaconOrder,mac->macBeaconOrder2,mac->macBeaconOrder3,wpan_pName(txPkt),p802_15_4macSA(txPkt),p802_15_4macDA(txPkt),ch->uid(),HDR_LRWPAN(txPkt)->uid,ch->size());
#endif
		if (mac->macBeaconOrder2 != 15)
		if (!mac->bcnRxT->busy())
			mac->bcnRxT->start();
		waitNextBeacon = true;
		return false;
	}
	else
	{
		bPeriodsLeft = -1;
		return true;
	}
}

//新信标发送过来后的响应
void CsmaCA802_15_4::newBeacon(char trx)
{
	//this function will be called by MAC each time a new beacon received or sent within the current PAN
	double rate,wtime;

	if (!mac->txAck)
		mac->plme_set_trx_state_request(p_RX_ON);	
	
	
	//
	if (bcnOtherT->busy())
		bcnOtherT->stop();

	//update values
	//更新信标使能状态
	//超帧结构由协调器定义，并且在网络层中使用MLME-START.request primitive请求原语进行配置。
	//两个连续信标间的持续时间，时间间隔（BI）, 由数值macBeaconOrder(BO)属性和aBaseSuperframeDuration常量使用下面的等式决定：
    //BI = aBaseSuperframeDuration × 2BO（symbols）
    //例如，给出aBaseSuperframeDuration的值是960 Symbols, 而BO的值是2，那么信标间隔BI将会是3840 symbols. 
	//“IEEE 802.15.4 standard document[2]”中提供了MAC的常量和属性。
    //在一个信标使能的网络中，BO可以是0-14中的任何值，如果BO的值被设置为15，网络将被认为是非信标使能，并且不使用任何超帧。
	//类似的，超帧激活时间段的长度叫做超帧持续时间（SD）,由下面等式计算得到：
	//SD = aBaseSuperframeDuration × 2SO（symbols）
	//其中SO是macSuperframeOrder属性的值。超帧持续时间SD不能超出信标间隔BI，因此,SO的值总是小于或等于BO。
	beaconEnabled = ((mac->mpib.macBeaconOrder != 15)||(mac->macBeaconOrder2 != 15));
	beaconOther = (mac->macBeaconOrder3 != 15);
	//先确定信标使能状态，然后执行reset
	reset();	
	rate = phy->getRate('s');
	bcnTxTime = mac->macBcnTxTime / rate;
	bcnRxTime = mac->macBcnRxTime / rate;
	bPeriod = aUnitBackoffPeriod / rate;

	if (waitNextBeacon)
	if ((txPkt)
        && (!backoffT->busy()))
	{
		assert(bPeriodsLeft >= 0);
		if (bPeriodsLeft == 0)
		{
			wtime = adjustTime(0.0);
			if (canProceed(wtime));
				backoffHandler();	//no need to resume backoff
		}
		else
		{
			wtime = adjustTime(0.0);
			wtime += bPeriodsLeft * bPeriod;
			if (canProceed(wtime));
				backoffT->start(wtime);
		}
	}
	waitNextBeacon = false;
}

void CsmaCA802_15_4::start(bool firsttime,Packet *pkt,bool ackreq)
{
	bool backoff;
	double rate,wtime,BI2;


	if (mac->txAck)
	{
		mac->backoffStatus = 0;
		txPkt = 0;
		return;
	}

	assert(backoffT->busy() == 0);
	if (firsttime)
	{
		beaconEnabled = ((mac->mpib.macBeaconOrder != 15)||(mac->macBeaconOrder2 != 15));
		beaconOther = (mac->macBeaconOrder3 != 15);
		reset();	
		assert(txPkt == 0);
		txPkt = pkt;
		ackReq = ackreq;
		rate = phy->getRate('s');
		bPeriod = aUnitBackoffPeriod / rate;
		if (beaconEnabled)
		{
			bcnTxTime = mac->macBcnTxTime / rate;
			bcnRxTime = mac->macBcnRxTime / rate;
			//it's possible we missed some beacons
			BI2 = (mac->sfSpec2.BI / phy->getRate('s'));
			if (mac->macBeaconOrder2 != 15)
			while (bcnRxTime + BI2 < CURRENT_TIME)
				bcnRxTime += BI2;
		}
	}

	wtime = (Random::random() % (1<<BE)) * bPeriod;
	wtime = adjustTime(wtime);
	backoff = true;
	if (beaconEnabled||beaconOther)
	{
		if (beaconEnabled)
		if (firsttime)
			wtime = mac->locateBoundary(mac->toParent(txPkt),wtime);
		if (!canProceed(wtime))		
			backoff = false;
	}
	if (backoff)
		backoffT->start(wtime);
}

void CsmaCA802_15_4::cancel(void)
{
	if (bcnOtherT->busy())
		bcnOtherT->stop();
	else if (backoffT->busy())
		backoffT->stop();
	else if (deferCCAT->busy())
		deferCCAT->stop();
	else
		mac->taskP.taskStatus(TP_CCA_csmaca) = false;
	txPkt = 0;
}

void CsmaCA802_15_4::backoffHandler(void)
{
	mac->taskP.taskStatus(TP_RX_ON_csmaca) = true;
	mac->plme_set_trx_state_request(p_RX_ON);
}

void CsmaCA802_15_4::RX_ON_confirm(PHYenum status)
{
	double now,wtime;

	if (status != p_RX_ON)
	{
		if (status == p_BUSY_TX)
			mac->taskP.taskStatus(TP_RX_ON_csmaca) = true;
		else
			backoffHandler();
		return;
	}

	//locate backoff boundary if needed
	now = CURRENT_TIME;
	if (beaconEnabled)
		wtime = mac->locateBoundary(mac->toParent(txPkt),0.0);
	else
		wtime = 0.0;

	if (wtime == 0.0)
	{
		mac->taskP.taskStatus(TP_CCA_csmaca) = true;
		phy->PLME_CCA_request();
	}
	else
		deferCCAT->start(wtime);
}

void CsmaCA802_15_4::bcnOtherHandler(void)
{
	newBeacon('R');
}

void CsmaCA802_15_4::deferCCAHandler(void)
{
	mac->taskP.taskStatus(TP_CCA_csmaca) = true;
	phy->PLME_CCA_request();
}

void CsmaCA802_15_4::CCA_confirm(PHYenum status)
{
	//This function should be called when mac receiving CCA_confirm.
	bool idle;

	idle = (status == p_IDLE)?1:0;	
	if (idle)
	{
		if ((!beaconEnabled)&&(!beaconOther))
		{
			txPkt = 0;
			mac->csmacaCallBack(p_IDLE);
		}
		else
		{
			if (beaconEnabled)
				CW--;
			else
				CW = 0;
			if (CW == 0)
			{
				//timing condition may not still hold -- check again
				if (canProceed(0.0, true))
				{
					txPkt = 0;
					mac->csmacaCallBack(p_IDLE);
				}
				else	//postpone until next beacon sent or received
				{
					if (beaconEnabled) CW = 2;
					bPeriodsLeft = 0;
				}
			}
			else	//perform CCA again
				backoffHandler();
		}
	}
	else	//busy
	{
		if (beaconEnabled) CW = 2;
		NB++;
		if (NB > mac->mpib.macMaxCSMABackoffs)
		{
			txPkt = 0;
			mac->csmacaCallBack(p_BUSY);
		}
		else	//backoff again
		{
			BE++;
			if (BE > aMaxBE)
				BE = aMaxBE;
			start(false);
		}
	}
}

// End of file: p802_15_4csmaca.cc
