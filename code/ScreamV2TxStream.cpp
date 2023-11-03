#include "RtpQueue.h"
#include "ScreamTx.h"

static const uint32_t kMinRtpQueueDiscardInterval_ntp = 16384; // 0.25s in NTP doain

ScreamV2Tx::Stream::Stream(ScreamV2Tx* parent_,
	RtpQueueIface* rtpQueue_,
	uint32_t ssrc_,
	float priority_,
	float minBitrate_,
	float startBitrate_,
	float maxBitrate_,
	float maxRtpQueueDelay_,
	bool isAdaptiveTargetRateScale_,
	float hysteresis_) {
	parent = parent_;
	rtpQueue = rtpQueue_;
	ssrc = ssrc_;
	targetPriority = priority_;
	targetPriorityInv = 1.0f / targetPriority;
	minBitrate = minBitrate_;
	maxBitrate = maxBitrate_;
	targetBitrate = std::min(maxBitrate, std::max(minBitrate, startBitrate_));
	targetBitrateH = targetBitrate;
	maxRtpQueueDelay = maxRtpQueueDelay_;
	isAdaptiveTargetRateScale = isAdaptiveTargetRateScale_;
	hysteresis = hysteresis_;
	credit = 0;
	creditLost = 0;
	bytesTransmitted = 0;
	bytesAcked = 0;
	bytesLost = 0;
	hiSeqAck = 0;
	hiSeqTx = 0;
	rateTransmitted = 0.0f;
	rateAcked = 0.0f;
	rateLost = 0.0f;
	rateCe = 0.0f;
	rateRtpAvg = 0.0f;
	lastRateUpdateT_ntp = 0;
	lastBitrateAdjustT_ntp = 0;
	lastTargetBitrateIUpdateT_ntp = 0;
	bytesRtp = 0;
	packetsRtp = 0;
	rateRtp = 0.0f;
	timeTxAck_ntp = 0;
	lastTransmitT_ntp = 0;
	numberOfUpdateRate = 0;
	cleared = 0;
	packetLost = 0;
	packetsCe = 0;
	packetsCe = 0;
	for (int n = 0; n < kRateUpDateSize; n++) {
		rateRtpHist[n] = 0.0f;
	}
	rateUpdateHistPtr = 0;
	targetRateScale = 1.0;
	isActive = false;
	lastFrameT_ntp = 0;
	initTime_ntp = 0;
	rtpQueueDiscard = false;
	lastRtpQueueDiscardT_ntp = 0;
	lastFullWindowT_ntp = 0;
	bytesLost = 0;
	bytesCe = 0;
	wasRepairLoss = false;
	repairLoss = false;
	for (int n = 0; n < kMaxTxPackets; n++)
		txPackets[n].isUsed = false;
	txPacketsPtr = 0;
	lossEpoch = false;
	frameSize = 0;
	frameSizeAcc = 0;
	frameSizeAvg = 0.0f;
	adaptivePacingRateScale = 1.0f;
	framePeriod = 0.02f;
}

/*
* Update the estimated max media rate
*/
void ScreamV2Tx::Stream::updateRate(uint32_t time_ntp) {
	if (lastRateUpdateT_ntp != 0 && parent->enableRateUpdate) {
		numberOfUpdateRate++;
		float tDelta = (time_ntp - lastRateUpdateT_ntp) * ntp2SecScaleFactor;
		rateTransmitted = bytesTransmitted * 8.0f / tDelta;
		rateAcked = bytesAcked * 8.0f / tDelta;
		rateLost = bytesLost * 8.0f / tDelta;
		rateCe = bytesCe * 8.0f / tDelta;
		rateRtpHist[rateUpdateHistPtr] = bytesRtp * 8.0f / tDelta;

		rateRtp = rateRtpHist[rateUpdateHistPtr];

		rateUpdateHistPtr = (rateUpdateHistPtr + 1) % kRateUpDateSize;

		rateRtpAvg = 0.0f;
		for (int n = 0; n < kRateUpDateSize; n++) {
			rateRtpAvg += rateRtpHist[n];
		}
		rateRtpAvg /= kRateUpDateSize;
		if (rateRtpAvg > 0 && isAdaptiveTargetRateScale && numberOfUpdateRate > kRateUpDateSize) {
			/*
			* Video coders are strange animals.. In certain cases the average bitrate is
			* consistently lower or higher than the target bitare. This additonal scaling compensates
			* for this anomaly.
			*/
			const float diff = targetBitrate * targetRateScale / rateRtpAvg;
			float alpha = 0.02f;
			targetRateScale *= (1.0f - alpha);
			targetRateScale += alpha * diff;
			targetRateScale = std::min(1.1f, std::max(0.8f, targetRateScale));
		}
		if (rateLost > 0) {
			lossEpoch = true;
		}
	}

	bytesTransmitted = 0;
	bytesAcked = 0;
	bytesRtp = 0;
	bytesLost = 0;
	bytesCe = 0;
	lastRateUpdateT_ntp = time_ntp;
}

/*
* Get the estimated maximum media rate
*/
float ScreamV2Tx::Stream::getMaxRate() {
	return std::max(rateTransmitted, rateAcked);
}

/*
* Get the stream that matches SSRC
*/
ScreamV2Tx::Stream* ScreamV2Tx::getStream(uint32_t ssrc, int& streamId) {
	for (int n = 0; n < nStreams; n++) {
		if (streams[n]->isMatch(ssrc)) {
			streamId = n;
			return streams[n];
		}
	}
	streamId = -1;
	return NULL;
}

void ScreamV2Tx::Stream::newMediaFrame(uint32_t time_ntp, int bytesRtp, bool isMarker) {
	frameSizeAcc += bytesRtp;
	/*
	 * Compute an adaptive pacing rate scale that allows the pacing rate to follow the frame sizes
	 *  so that packets are paced out faster when a large frame is generated by the encoder.
	 * This reduces the RTP queue but can potentially give a larger queue or more L4S marking in the network
	 * Pacing rate scaling is also increased if the RTP queue grows
	 */
	if (isMarker) {
		/*
		* Compute average frame period
		*/
		const float alpha = 0.1f;
		if (lastFrameT_ntp != 0) {
			float dT = (time_ntp - lastFrameT_ntp) * ntp2SecScaleFactor;
			framePeriod = dT * (1 - alpha) + framePeriod * alpha;
		}
		lastFrameT_ntp = time_ntp;



		frameSizeAvg = targetBitrate * framePeriod / 8.0f;

		frameSize = std::max(rtpQueue->bytesInQueue(), frameSizeAcc);

		frameSizeAcc = 0;


		if (frameSizeAvg > 500.0f) {
			adaptivePacingRateScale = std::min(parent->maxAdaptivePacingRateScale, std::max(1.0f, frameSize / frameSizeAvg));
		}
		else {
			adaptivePacingRateScale = 1.0f;
		}
		updateTargetBitrate(time_ntp);
	}
}

/*
* Get the target bitrate.
* This function returns a value -1 if loss of RTP packets is detected,
*  either because of loss in network or RTP queue discard
*/
float ScreamV2Tx::Stream::getTargetBitrate() {

	bool requestRefresh = isRtpQueueDiscard() || repairLoss;
	repairLoss = false;
	if (requestRefresh && !wasRepairLoss) {
		wasRepairLoss = true;
		return -1.0;
	}
	float rate = targetRateScale * targetBitrateH;
	wasRepairLoss = false;
	return rate;
}

/*
* Update target bitrate
*/
void ScreamV2Tx::Stream::updateTargetBitrate(uint32_t time_ntp) {

	isActive = true;

	if (initTime_ntp == 0) {
		/*
		 * Initialize if the first time
		 */
		initTime_ntp = time_ntp;
		lastRtpQueueDiscardT_ntp = time_ntp;
	}
	if (lastBitrateAdjustT_ntp == 0) {
		lastBitrateAdjustT_ntp = time_ntp;
	}

	float rtpQueueDelay = rtpQueue->getDelay(time_ntp * ntp2SecScaleFactor);
	if (rtpQueueDelay > maxRtpQueueDelay &&
		(time_ntp - lastRtpQueueDiscardT_ntp > kMinRtpQueueDiscardInterval_ntp)) {
		/*
		* RTP queue is cleared as it is becoming too large,
		* Function is however disabled initially as there is no reliable estimate of the
		* throughput in the initial phase.
		*/
		int seqNrOfNextRtp = rtpQueue->seqNrOfNextRtp();
		int seqNrOfLastRtp = rtpQueue->seqNrOfLastRtp();
		int pak_diff = (seqNrOfLastRtp == -1) ? -1 : ((seqNrOfLastRtp >= hiSeqTx) ? (seqNrOfLastRtp - hiSeqTx) : seqNrOfLastRtp + 0xffff - hiSeqTx);

		int cur_cleared = rtpQueue->clear();
		cerr << parent->logTag << " rtpQueueDelay " << rtpQueueDelay << " too large 1 " << time_ntp / 65536.0f << " RTP queue " << cur_cleared <<
			" packets discarded for SSRC " << ssrc << " hiSeqTx " << hiSeqTx << " hiSeqAckendl " << hiSeqAck <<
			" seqNrOfNextRtp " << seqNrOfNextRtp << " seqNrOfLastRtp " << seqNrOfLastRtp << " diff " << pak_diff << endl;
		cleared += cur_cleared;
		rtpQueueDiscard = true;
		lossEpoch = true;

		lastRtpQueueDiscardT_ntp = time_ntp;
		targetRateScale = 1.0;
		rtpQueueDelay = rtpQueue->getDelay(time_ntp * ntp2SecScaleFactor);
	}


	/*
	* Rate is computed based CWND and RTT
	*/

	/*
	* In the multistream solution, the CWND is split between the streams according to the
	* priorities
	*  cwndShare = cwnd * priority[stream] /sum(priority[0]..priority[N-1])
	*/
	float cwndShare = parent->cwnd;
	float prioritySum = 0.0f;
	float priorityShare = 1.0;
	for (int n = 0; n < parent->nStreams; n++) {
		Stream* stream = parent->streams[n];
		if (stream->isActive)
			prioritySum += stream->targetPriority;
	}
	if (prioritySum > 0) {
		priorityShare = targetPriority / prioritySum;
		cwndShare *= priorityShare;
	}

	float tmp = 1.0f;

	/*
	 * Scale down rate slighty when the congestion window is very small compared to mss
	 * Note that at very low bitrates it is necessary to reduce the MTU also
	 */
	tmp *= 1.0f - std::min(0.8f, std::max(0.0f, parent->cwndRatio - 0.1f));

	float sRtt = parent->sRtt;

	/*
	* Compute target bitrate.
	*/
	tmp *= 8 * cwndShare / std::max(0.001f, std::min(0.2f, sRtt));
	targetBitrate = tmp;

	targetBitrate = std::min(maxBitrate, std::max(minBitrate, targetBitrate));

	/*
	* Update targetBitrateH
	*/
	float diff = (targetBitrate * targetRateScale - targetBitrateH) / targetBitrateH;
	if (diff > hysteresis || diff < -hysteresis / 4) {
		/*
		* Update target bitrate to video encoder only if bitrate varies
		*  enough, this prevents excessive rate signaling to the video encoder
		*  that can mess up the rate control loop in the encoder
		*/
		targetBitrateH = targetBitrate * targetRateScale;
	}
}

bool ScreamV2Tx::Stream::isRtpQueueDiscard() {
	bool tmp = rtpQueueDiscard;
	rtpQueueDiscard = false;
	return tmp;
}

bool ScreamV2Tx::Stream::isLossEpoch() {
	bool tmp = lossEpoch;
	lossEpoch = false;
	return tmp;
}