/*
 * Device support for Ettus Research UHD driver
 *
 * Copyright 2010,2011 Free Software Foundation, Inc.
 * Copyright (C) 2015 Ettus Research LLC
 *
 * Author: Tom Tsou <tom.tsou@ettus.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 * See the COPYING file in the main directory for details.
 */

#include <map>
#include "radioDevice.h"
#include "Threads.h"
#include "Logger.h"
#include <uhd/version.hpp>
#include <uhd/property_tree.hpp>
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/utils/thread_priority.hpp>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifndef USE_UHD_3_11
#include <uhd/utils/msg.hpp>
#endif

#define USRP_TX_AMPL     0.3
#define UMTRX_TX_AMPL    0.7
#define LIMESDR_TX_AMPL  0.3
#define SAMPLE_BUF_SZ    (1 << 20)

/*
 * UHD timeout value on streaming (re)start
 *
 * Allow some time for streaming to commence after the start command is issued,
 * but consider a wait beyond one second to be a definite error condition.
 */
#define UHD_RESTART_TIMEOUT     1.0

/*
 * UmTRX specific settings
 */
#define UMTRX_VGA1_DEF   -18

enum uhd_dev_type {
	USRP1,
	USRP2,
	B100,
	B200,
	B210,
	B2XX_MCBTS,
	E1XX,
	E3XX,
	X3XX,
	UMTRX,
	LIMESDR,
};

/*
 * USRP version dependent device timings
 */
#if defined(USE_UHD_3_9) || defined(USE_UHD_3_11)
#define B2XX_TIMING_1SPS	1.7153e-4
#define B2XX_TIMING_4SPS	1.1696e-4
#define B2XX_TIMING_4_4SPS	6.18462e-5
#define B2XX_TIMING_MCBTS	7e-5
#else
#define B2XX_TIMING_1SPS	9.9692e-5
#define B2XX_TIMING_4SPS	6.9248e-5
#define B2XX_TIMING_4_4SPS	4.52308e-5
#define B2XX_TIMING_MCBTS	6.42452e-5
#endif

/*
 * Tx / Rx sample offset values. In a perfect world, there is no group delay
 * though analog components, and behaviour through digital filters exactly
 * matches calculated values. In reality, there are unaccounted factors,
 * which are captured in these empirically measured (using a loopback test)
 * timing correction values.
 *
 * Notes:
 *   USRP1 with timestamps is not supported by UHD.
 */

/* Device Type, Tx-SPS, Rx-SPS */
typedef std::tuple<uhd_dev_type, int, int> dev_key;

/* Device parameter descriptor */
struct dev_desc {
	unsigned channels;
	double mcr;
	double rate;
	double offset;
	std::string str;
};

static const std::map<dev_key, dev_desc> dev_param_map {
	{ std::make_tuple(USRP2, 1, 1), { 1, 0.0,  390625,  1.2184e-4,  "N2XX 1 SPS"         } },
	{ std::make_tuple(USRP2, 4, 1), { 1, 0.0,  390625,  7.6547e-5,  "N2XX 4/1 Tx/Rx SPS" } },
	{ std::make_tuple(USRP2, 4, 4), { 1, 0.0,  390625,  4.6080e-5,  "N2XX 4 SPS"         } },
	{ std::make_tuple(B100,  1, 1), { 1, 0.0,  400000,  1.2104e-4,  "B100 1 SPS"         } },
	{ std::make_tuple(B100,  4, 1), { 1, 0.0,  400000,  7.9307e-5,  "B100 4/1 Tx/Rx SPS" } },
	{ std::make_tuple(B200,  1, 1), { 1, 26e6, GSMRATE, B2XX_TIMING_1SPS, "B200 1 SPS"   } },
	{ std::make_tuple(B200,  4, 1), { 1, 26e6, GSMRATE, B2XX_TIMING_4SPS, "B200 4/1 Tx/Rx SPS" } },
	{ std::make_tuple(B200,  4, 4), { 1, 26e6, GSMRATE, B2XX_TIMING_4_4SPS, "B200 4 SPS" } },
	{ std::make_tuple(B210,  1, 1), { 2, 26e6, GSMRATE, B2XX_TIMING_1SPS, "B210 1 SPS"    } },
	{ std::make_tuple(B210,  4, 1), { 2, 26e6, GSMRATE, B2XX_TIMING_4SPS, "B210 4/1 Tx/Rx SPS" } },
	{ std::make_tuple(B210,  4, 4), { 2, 26e6, GSMRATE, B2XX_TIMING_4_4SPS, "B210 4 SPS" } },
	{ std::make_tuple(E1XX,  1, 1), { 1, 52e6, GSMRATE, 9.5192e-5,  "E1XX 1 SPS"         } },
	{ std::make_tuple(E1XX,  4, 1), { 1, 52e6, GSMRATE, 6.5571e-5,  "E1XX 4/1 Tx/Rx SPS" } },
	{ std::make_tuple(E3XX,  1, 1), { 2, 26e6, GSMRATE, 1.8462e-4,  "E3XX 1 SPS"         } },
	{ std::make_tuple(E3XX,  4, 1), { 2, 26e6, GSMRATE, 1.2923e-4,  "E3XX 4/1 Tx/Rx SPS" } },
	{ std::make_tuple(X3XX,  1, 1), { 2, 0.0,  390625,  1.5360e-4,  "X3XX 1 SPS"         } },
	{ std::make_tuple(X3XX,  4, 1), { 2, 0.0,  390625,  1.1264e-4,  "X3XX 4/1 Tx/Rx SPS" } },
	{ std::make_tuple(X3XX,  4, 4), { 2, 0.0,  390625,  5.6567e-5,  "X3XX 4 SPS"         } },
	{ std::make_tuple(UMTRX, 1, 1), { 2, 0.0,  GSMRATE, 9.9692e-5,  "UmTRX 1 SPS"        } },
	{ std::make_tuple(UMTRX, 4, 1), { 2, 0.0,  GSMRATE, 7.3846e-5,  "UmTRX 4/1 Tx/Rx SPS"} },
	{ std::make_tuple(UMTRX, 4, 4), { 2, 0.0,  GSMRATE, 5.1503e-5,  "UmTRX 4 SPS"        } },
	{ std::make_tuple(LIMESDR, 4, 4), { 1, GSMRATE*32, GSMRATE, 8.9e-5, "LimeSDR 4 SPS"  } },
	{ std::make_tuple(B2XX_MCBTS, 4, 4), { 1, 51.2e6, MCBTS_SPACING*4, B2XX_TIMING_MCBTS, "B200/B210 4 SPS Multi-ARFCN" } },
};

/*
    Sample Buffer - Allows reading and writing of timed samples using osmo-trx
                    or UHD style timestamps. Time conversions are handled
                    internally or accessable through the static convert calls.
*/
class smpl_buf {
public:
	/** Sample buffer constructor
	    @param len number of 32-bit samples the buffer should hold
	    @param rate sample clockrate
	    @param timestamp
	*/
	smpl_buf(size_t len, double rate);
	~smpl_buf();

	/** Query number of samples available for reading
	    @param timestamp time of first sample
	    @return number of available samples or error
	*/
	ssize_t avail_smpls(TIMESTAMP timestamp) const;
	ssize_t avail_smpls(uhd::time_spec_t timestamp) const;

	/** Read and write
	    @param buf pointer to buffer
	    @param len number of samples desired to read or write
	    @param timestamp time of first stample
	    @return number of actual samples read or written or error
	*/
	ssize_t read(void *buf, size_t len, TIMESTAMP timestamp);
	ssize_t read(void *buf, size_t len, uhd::time_spec_t timestamp);
	ssize_t write(void *buf, size_t len, TIMESTAMP timestamp);
	ssize_t write(void *buf, size_t len, uhd::time_spec_t timestamp);

	/** Buffer status string
	    @return a formatted string describing internal buffer state
	*/
	std::string str_status(size_t ts) const;

	/** Formatted error string
	    @param code an error code
	    @return a formatted error string
	*/
	static std::string str_code(ssize_t code);

	enum err_code {
		ERROR_TIMESTAMP = -1,
		ERROR_READ = -2,
		ERROR_WRITE = -3,
		ERROR_OVERFLOW = -4
	};

private:
	uint32_t *data;
	size_t buf_len;

	double clk_rt;

	TIMESTAMP time_start;
	TIMESTAMP time_end;

	size_t data_start;
	size_t data_end;
};

/*
    uhd_device - UHD implementation of the Device interface. Timestamped samples
                are sent to and received from the device. An intermediate buffer
                on the receive side collects and aligns packets of samples.
                Events and errors such as underruns are reported asynchronously
                by the device and received in a separate thread.
*/
class uhd_device : public RadioDevice {
public:
	uhd_device(size_t tx_sps, size_t rx_sps, InterfaceType type,
		   size_t chans, double offset,
		   const std::vector<std::string>& tx_paths,
		   const std::vector<std::string>& rx_paths);
	~uhd_device();

	int open(const std::string &args, int ref, bool swap_channels);
	bool start();
	bool stop();
	bool restart();
	void setPriority(float prio);
	enum TxWindowType getWindowType() { return tx_window; }

	int readSamples(std::vector<short *> &bufs, int len, bool *overrun,
			TIMESTAMP timestamp, bool *underrun, unsigned *RSSI);

	int writeSamples(std::vector<short *> &bufs, int len, bool *underrun,
			 TIMESTAMP timestamp, bool isControl);

	bool updateAlignment(TIMESTAMP timestamp);

	bool setTxFreq(double wFreq, size_t chan);
	bool setRxFreq(double wFreq, size_t chan);

	TIMESTAMP initialWriteTimestamp();
	TIMESTAMP initialReadTimestamp();

	double fullScaleInputValue();
	double fullScaleOutputValue();

	double setRxGain(double db, size_t chan);
	double getRxGain(size_t chan);
	double maxRxGain(void) { return rx_gain_max; }
	double minRxGain(void) { return rx_gain_min; }

	double setTxGain(double db, size_t chan);
	double maxTxGain(void) { return tx_gain_max; }
	double minTxGain(void) { return tx_gain_min; }

	double getTxFreq(size_t chan);
	double getRxFreq(size_t chan);
	double getRxFreq();

	bool setRxAntenna(const std::string &ant, size_t chan);
	std::string getRxAntenna(size_t chan);
	bool setTxAntenna(const std::string &ant, size_t chan);
	std::string getTxAntenna(size_t chan);

	inline double getSampleRate() { return tx_rate; }
	inline double numberRead() { return rx_pkt_cnt; }
	inline double numberWritten() { return 0; }

	/** Receive and process asynchronous message
	    @return true if message received or false on timeout or error
	*/
	bool recv_async_msg();

	enum err_code {
		ERROR_TIMING = -1,
		ERROR_TIMEOUT = -2,
		ERROR_UNRECOVERABLE = -3,
		ERROR_UNHANDLED = -4,
	};

private:
	uhd::usrp::multi_usrp::sptr usrp_dev;
	uhd::tx_streamer::sptr tx_stream;
	uhd::rx_streamer::sptr rx_stream;
	enum TxWindowType tx_window;
	enum uhd_dev_type dev_type;

	size_t tx_sps, rx_sps, chans;
	double tx_rate, rx_rate;

	double tx_gain_min, tx_gain_max;
	double rx_gain_min, rx_gain_max;
	double offset;

	std::vector<double> tx_gains, rx_gains;
	std::vector<double> tx_freqs, rx_freqs;
	std::vector<std::string> tx_paths, rx_paths;
	size_t tx_spp, rx_spp;

	bool started;
	bool aligned;

	size_t rx_pkt_cnt;
	size_t drop_cnt;
	uhd::time_spec_t prev_ts;

	TIMESTAMP ts_initial, ts_offset;
	std::vector<smpl_buf *> rx_buffers;

	void init_gains();
	void set_channels(bool swap);
	void set_rates();
	bool set_antennas();
	bool parse_dev_type();
	bool flush_recv(size_t num_pkts);
	int check_rx_md_err(uhd::rx_metadata_t &md, ssize_t num_smpls);

	std::string str_code(uhd::rx_metadata_t metadata);
	std::string str_code(uhd::async_metadata_t metadata);

	uhd::tune_request_t select_freq(double wFreq, size_t chan, bool tx);
	bool set_freq(double freq, size_t chan, bool tx);

	Thread *async_event_thrd;
	InterfaceType iface;
	Mutex tune_lock;
};

void *async_event_loop(uhd_device *dev)
{
	dev->setPriority(0.43);

	while (1) {
		dev->recv_async_msg();
		pthread_testcancel();
	}

	return NULL;
}

#ifndef USE_UHD_3_11
/*
    Catch and drop underrun 'U' and overrun 'O' messages from stdout
    since we already report using the logging facility. Direct
    everything else appropriately.
 */
void uhd_msg_handler(uhd::msg::type_t type, const std::string &msg)
{
	switch (type) {
	case uhd::msg::status:
		LOG(INFO) << msg;
		break;
	case uhd::msg::warning:
		LOG(WARNING) << msg;
		break;
	case uhd::msg::error:
		LOG(ERR) << msg;
		break;
	case uhd::msg::fastpath:
		break;
	}
}
#endif

static void thread_enable_cancel(bool cancel)
{
	cancel ? pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL) :
		 pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
}

uhd_device::uhd_device(size_t tx_sps, size_t rx_sps,
		       InterfaceType iface, size_t chans, double offset,
		       const std::vector<std::string>& tx_paths,
		       const std::vector<std::string>& rx_paths)
	: tx_gain_min(0.0), tx_gain_max(0.0),
	  rx_gain_min(0.0), rx_gain_max(0.0),
	  tx_spp(0), rx_spp(0),
	  started(false), aligned(false), rx_pkt_cnt(0), drop_cnt(0),
	  prev_ts(0,0), ts_initial(0), ts_offset(0), async_event_thrd(NULL)
{
	this->tx_sps = tx_sps;
	this->rx_sps = rx_sps;
	this->chans = chans;
	this->offset = offset;
	this->iface = iface;
	this->tx_paths = tx_paths;
	this->rx_paths = rx_paths;
}

uhd_device::~uhd_device()
{
	stop();

	for (size_t i = 0; i < rx_buffers.size(); i++)
		delete rx_buffers[i];
}

void uhd_device::init_gains()
{
	uhd::gain_range_t range;

	if (dev_type == UMTRX) {
		std::vector<std::string> gain_stages = usrp_dev->get_tx_gain_names(0);
		if (gain_stages[0] == "VGA") {
			LOG(WARNING) << "Update your UHD version for a proper Tx gain support";
		}
		if (gain_stages[0] == "VGA" || gain_stages[0] == "PA") {
			range = usrp_dev->get_tx_gain_range();
			tx_gain_min = range.start();
			tx_gain_max = range.stop();
		} else {
			range = usrp_dev->get_tx_gain_range("VGA2");
			tx_gain_min = UMTRX_VGA1_DEF + range.start();
			tx_gain_max = UMTRX_VGA1_DEF + range.stop();
		}
	} else {
		range = usrp_dev->get_tx_gain_range();
		tx_gain_min = range.start();
		tx_gain_max = range.stop();
	}
	LOG(INFO) << "Supported Tx gain range [" << tx_gain_min << "; " << tx_gain_max << "]";

	range = usrp_dev->get_rx_gain_range();
	rx_gain_min = range.start();
	rx_gain_max = range.stop();
	LOG(INFO) << "Supported Rx gain range [" << rx_gain_min << "; " << rx_gain_max << "]";

	for (size_t i = 0; i < tx_gains.size(); i++) {
		double gain = (tx_gain_min + tx_gain_max) / 2;
		LOG(INFO) << "Default setting Tx gain for channel " << i << " to " << gain;
		usrp_dev->set_tx_gain(gain, i);
		tx_gains[i] = usrp_dev->get_tx_gain(i);
	}

	for (size_t i = 0; i < rx_gains.size(); i++) {
		double gain = (rx_gain_min + rx_gain_max) / 2;
		LOG(INFO) << "Default setting Rx gain for channel " << i << " to " << gain;
		usrp_dev->set_rx_gain(gain, i);
		rx_gains[i] = usrp_dev->get_rx_gain(i);
	}

	return;

}

void uhd_device::set_rates()
{
	dev_desc desc = dev_param_map.at(dev_key(dev_type, tx_sps, rx_sps));
	if (desc.mcr != 0.0)
		usrp_dev->set_master_clock_rate(desc.mcr);

	tx_rate = (dev_type != B2XX_MCBTS) ? desc.rate * tx_sps : desc.rate;
	rx_rate = (dev_type != B2XX_MCBTS) ? desc.rate * rx_sps : desc.rate;

	usrp_dev->set_tx_rate(tx_rate);
	usrp_dev->set_rx_rate(rx_rate);
	tx_rate = usrp_dev->get_tx_rate();
	rx_rate = usrp_dev->get_rx_rate();

	ts_offset = static_cast<TIMESTAMP>(desc.offset * rx_rate);
	LOG(INFO) << "Rates configured for " << desc.str;
}

bool uhd_device::set_antennas()
{
	unsigned int i;

	for (i = 0; i < tx_paths.size(); i++) {
		if (tx_paths[i] == "")
			continue;
		LOG(DEBUG) << "Configuring channel " << i << " with antenna " << tx_paths[i];
		if (!setTxAntenna(tx_paths[i], i)) {
			LOG(ALERT) << "Failed configuring channel " << i << " with antenna " << tx_paths[i];
			return false;
		}
	}

	for (i = 0; i < rx_paths.size(); i++) {
		if (rx_paths[i] == "")
			continue;
		LOG(DEBUG) << "Configuring channel " << i << " with antenna " << rx_paths[i];
		if (!setRxAntenna(rx_paths[i], i)) {
			LOG(ALERT) << "Failed configuring channel " << i << " with antenna " << rx_paths[i];
			return false;
		}
	}
	LOG(INFO) << "Antennas configured successfully";
	return true;
}

double uhd_device::setTxGain(double db, size_t chan)
{
	if (iface == MULTI_ARFCN)
		chan = 0;

	if (chan >= tx_gains.size()) {
		LOG(ALERT) << "Requested non-existent channel" << chan;
		return 0.0f;
	}

	if (dev_type == UMTRX) {
		std::vector<std::string> gain_stages = usrp_dev->get_tx_gain_names(0);
		if (gain_stages[0] == "VGA" || gain_stages[0] == "PA") {
			usrp_dev->set_tx_gain(db, chan);
		} else {
			// New UHD versions support split configuration of
			// Tx gain stages. We utilize this to set the gain
			// configuration, optimal for the Tx signal quality.
			// From our measurements, VGA1 must be 18dB plus-minus
			// one and VGA2 is the best when 23dB or lower.
			usrp_dev->set_tx_gain(UMTRX_VGA1_DEF, "VGA1", chan);
			usrp_dev->set_tx_gain(db-UMTRX_VGA1_DEF, "VGA2", chan);
		}
	} else {
		usrp_dev->set_tx_gain(db, chan);
	}

	tx_gains[chan] = usrp_dev->get_tx_gain(chan);

	LOG(INFO) << "Set TX gain to " << tx_gains[chan] << "dB (asked for " << db << "dB)";

	return tx_gains[chan];
}

double uhd_device::setRxGain(double db, size_t chan)
{
	if (chan >= rx_gains.size()) {
		LOG(ALERT) << "Requested non-existent channel " << chan;
		return 0.0f;
	}

	usrp_dev->set_rx_gain(db, chan);
	rx_gains[chan] = usrp_dev->get_rx_gain(chan);

	LOG(INFO) << "Set RX gain to " << rx_gains[chan] << "dB (asked for " << db << "dB)";

	return rx_gains[chan];
}

double uhd_device::getRxGain(size_t chan)
{
	if (iface == MULTI_ARFCN)
		chan = 0;

	if (chan >= rx_gains.size()) {
		LOG(ALERT) << "Requested non-existent channel " << chan;
		return 0.0f;
	}

	return rx_gains[chan];
}

/*
    Parse the UHD device tree and mboard name to find out what device we're
    dealing with. We need the window type so that the transceiver knows how to
    deal with the transport latency. Reject the USRP1 because UHD doesn't
    support timestamped samples with it.
 */
bool uhd_device::parse_dev_type()
{
	uhd::property_tree::sptr prop_tree = usrp_dev->get_device()->get_tree();
	std::string devString = prop_tree->access<std::string>("/name").get();
	std::string mboardString = usrp_dev->get_mboard_name();

	const std::map<std::string, std::pair<uhd_dev_type, TxWindowType>> devStringMap {
		{ "B100",     { B100,    TX_WINDOW_USRP1 } },
		{ "B200",     { B200,    TX_WINDOW_USRP1 } },
		{ "B200mini", { B200,    TX_WINDOW_USRP1 } },
		{ "B205mini", { B200,    TX_WINDOW_USRP1 } },
		{ "B210",     { B210,    TX_WINDOW_USRP1 } },
		{ "E100",     { E1XX,    TX_WINDOW_FIXED } },
		{ "E110",     { E1XX,    TX_WINDOW_FIXED } },
		{ "E310",     { E3XX,    TX_WINDOW_FIXED } },
		{ "E3XX",     { E3XX,    TX_WINDOW_FIXED } },
		{ "X300",     { X3XX,    TX_WINDOW_FIXED } },
		{ "X310",     { X3XX,    TX_WINDOW_FIXED } },
		{ "USRP2",    { USRP2,   TX_WINDOW_FIXED } },
		{ "UmTRX",    { UMTRX,   TX_WINDOW_FIXED } },
		{ "LimeSDR",  { LIMESDR, TX_WINDOW_FIXED } },
	};

	// Compare UHD motherboard and device strings */
	auto mapIter = devStringMap.begin();
	while (mapIter != devStringMap.end()) {
		if (devString.find(mapIter->first) != std::string::npos ||
		    mboardString.find(mapIter->first) != std::string::npos) {
			dev_type = std::get<0>(mapIter->second);
			tx_window = std::get<1>(mapIter->second);
			return true;
		}
		mapIter++;
	}

	LOG(ALERT) << "Unsupported device " << devString;
	return false;
}

/*
 * Check for UHD version > 3.9.0 for E3XX support
 */
static bool uhd_e3xx_version_chk()
{
	std::string ver = uhd::get_version_string();
	std::string major_str(ver.begin(), ver.begin() + 3);
	std::string minor_str(ver.begin() + 4, ver.begin() + 7);

	int major_val = atoi(major_str.c_str());
	int minor_val = atoi(minor_str.c_str());

	if (major_val < 3)
		return false;
	if (minor_val < 9)
		return false;

	return true;
}

void uhd_device::set_channels(bool swap)
{
	if (iface == MULTI_ARFCN) {
		if (dev_type != B200 && dev_type != B210)
			throw std::invalid_argument("Device does not support MCBTS");
		dev_type = B2XX_MCBTS;
		chans = 1;
	}

	if (chans > dev_param_map.at(dev_key(dev_type, tx_sps, rx_sps)).channels)
		throw std::invalid_argument("Device does not support number of requested channels");

	std::string subdev_string;
	switch (dev_type) {
	case B210:
	case E3XX:
		if (chans == 1)
			subdev_string = swap ? "A:B" : "A:A";
		else if (chans == 2)
			subdev_string = swap ? "A:B A:A" : "A:A A:B";
		break;
	case X3XX:
	case UMTRX:
		if (chans == 1)
			subdev_string = swap ? "B:0" : "A:0";
		else if (chans == 2)
			subdev_string = swap ? "B:0 A:0" : "A:0 B:0";
		break;
	default:
		break;
	}

	if (!subdev_string.empty()) {
		uhd::usrp::subdev_spec_t spec(subdev_string);
		usrp_dev->set_tx_subdev_spec(spec);
		usrp_dev->set_rx_subdev_spec(spec);
	}
}

int uhd_device::open(const std::string &args, int ref, bool swap_channels)
{
	const char *refstr;

	// Find UHD devices
	uhd::device_addr_t addr(args);
	uhd::device_addrs_t dev_addrs = uhd::device::find(addr);
	if (dev_addrs.size() == 0) {
		LOG(ALERT) << "No UHD devices found with address '" << args << "'";
		return -1;
	}

	// Use the first found device
	LOG(INFO) << "Using discovered UHD device " << dev_addrs[0].to_string();
	try {
		usrp_dev = uhd::usrp::multi_usrp::make(addr);
	} catch(...) {
		LOG(ALERT) << "UHD make failed, device " << args;
		return -1;
	}

	// Check for a valid device type and set bus type
	if (!parse_dev_type())
		return -1;

	if ((dev_type == E3XX) && !uhd_e3xx_version_chk()) {
		LOG(ALERT) << "E3XX requires UHD 003.009.000 or greater";
		return -1;
	}

	try {
		set_channels(swap_channels);
        } catch (const std::exception &e) {
		LOG(ALERT) << "Channel setting failed - " << e.what();
		return -1;
	}

	if (!set_antennas()) {
		LOG(ALERT) << "UHD antenna setting failed";
		return -1;
	}

	tx_freqs.resize(chans);
	rx_freqs.resize(chans);
	tx_gains.resize(chans);
	rx_gains.resize(chans);
	rx_buffers.resize(chans);

	switch (ref) {
	case REF_INTERNAL:
		refstr = "internal";
		break;
	case REF_EXTERNAL:
		refstr = "external";
		break;
	case REF_GPS:
		refstr = "gpsdo";
		break;
	default:
		LOG(ALERT) << "Invalid reference type";
		return -1;
	}

	usrp_dev->set_clock_source(refstr);

	try {
		set_rates();
        } catch (const std::exception &e) {
		LOG(ALERT) << "UHD rate setting failed - " << e.what();
		return -1;
	}

	// Set RF frontend bandwidth
	if (dev_type == UMTRX) {
		// Setting LMS6002D LPF to 500kHz gives us the best signal quality
		for (size_t i = 0; i < chans; i++) {
			usrp_dev->set_tx_bandwidth(500*1000*2, i);
			usrp_dev->set_rx_bandwidth(500*1000*2, i);
		}
	} else if (dev_type == LIMESDR) {
		for (size_t i = 0; i < chans; i++) {
			usrp_dev->set_tx_bandwidth(5e6, i);
			usrp_dev->set_rx_bandwidth(5e6, i);
		}
	}

	/* Create TX and RX streamers */
	uhd::stream_args_t stream_args("sc16");
	for (size_t i = 0; i < chans; i++)
		stream_args.channels.push_back(i);

	tx_stream = usrp_dev->get_tx_stream(stream_args);
	rx_stream = usrp_dev->get_rx_stream(stream_args);

	/* Number of samples per over-the-wire packet */
	tx_spp = tx_stream->get_max_num_samps();
	rx_spp = rx_stream->get_max_num_samps();

	// Create receive buffer
	size_t buf_len = SAMPLE_BUF_SZ / sizeof(uint32_t);
	for (size_t i = 0; i < rx_buffers.size(); i++)
		rx_buffers[i] = new smpl_buf(buf_len, rx_rate);

	// Initialize and shadow gain values
	init_gains();

	// Print configuration
	LOG(INFO) << "\n" << usrp_dev->get_pp_string();

	if (iface == MULTI_ARFCN)
		return MULTI_ARFCN;

	switch (dev_type) {
	case B100:
		return RESAMP_64M;
	case USRP2:
	case X3XX:
		return RESAMP_100M;
	case B200:
	case B210:
	case E1XX:
	case E3XX:
	case LIMESDR:
	default:
		break;
	}

	return NORMAL;
}

bool uhd_device::flush_recv(size_t num_pkts)
{
	uhd::rx_metadata_t md;
	size_t num_smpls;
	float timeout = UHD_RESTART_TIMEOUT;

	std::vector<std::vector<short> >
		pkt_bufs(chans, std::vector<short>(2 * rx_spp));

	std::vector<short *> pkt_ptrs;
	for (size_t i = 0; i < pkt_bufs.size(); i++)
		pkt_ptrs.push_back(&pkt_bufs[i].front());

	ts_initial = 0;
	while (!ts_initial || (num_pkts-- > 0)) {
		num_smpls = rx_stream->recv(pkt_ptrs, rx_spp, md,
					    timeout, true);
		if (!num_smpls) {
			switch (md.error_code) {
			case uhd::rx_metadata_t::ERROR_CODE_TIMEOUT:
				LOG(ALERT) << "Device timed out";
				return false;
			default:
				continue;
			}
		}

		ts_initial = md.time_spec.to_ticks(rx_rate);
	}

	LOG(INFO) << "Initial timestamp " << ts_initial << std::endl;

	return true;
}

bool uhd_device::restart()
{
	/* Allow 100 ms delay to align multi-channel streams */
	double delay = 0.1;

	aligned = false;

	uhd::time_spec_t current = usrp_dev->get_time_now();

	uhd::stream_cmd_t cmd = uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS;
	cmd.stream_now = false;
	cmd.time_spec = uhd::time_spec_t(current.get_real_secs() + delay);

	usrp_dev->issue_stream_cmd(cmd);

	return flush_recv(10);
}

bool uhd_device::start()
{
	LOG(INFO) << "Starting USRP...";

	if (started) {
		LOG(ERR) << "Device already started";
		return false;
	}

#ifndef USE_UHD_3_11
	// Register msg handler
	uhd::msg::register_handler(&uhd_msg_handler);
#endif
	// Start asynchronous event (underrun check) loop
	async_event_thrd = new Thread();
	async_event_thrd->start((void * (*)(void*))async_event_loop, (void*)this);

	// Start streaming
	if (!restart())
		return false;

	// Display usrp time
	double time_now = usrp_dev->get_time_now().get_real_secs();
	LOG(INFO) << "The current time is " << time_now << " seconds";

	started = true;
	return true;
}

bool uhd_device::stop()
{
	if (!started)
		return false;

	uhd::stream_cmd_t stream_cmd =
		uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS;

	usrp_dev->issue_stream_cmd(stream_cmd);

	async_event_thrd->cancel();
	async_event_thrd->join();
	delete async_event_thrd;

	started = false;
	return true;
}

void uhd_device::setPriority(float prio)
{
	uhd::set_thread_priority_safe(prio);
	return;
}

int uhd_device::check_rx_md_err(uhd::rx_metadata_t &md, ssize_t num_smpls)
{
	if (!num_smpls) {
		LOG(ERR) << str_code(md);

		switch (md.error_code) {
		case uhd::rx_metadata_t::ERROR_CODE_TIMEOUT:
			LOG(ALERT) << "UHD: Receive timed out";
			return ERROR_TIMEOUT;
		case uhd::rx_metadata_t::ERROR_CODE_OVERFLOW:
		case uhd::rx_metadata_t::ERROR_CODE_LATE_COMMAND:
		case uhd::rx_metadata_t::ERROR_CODE_BROKEN_CHAIN:
		case uhd::rx_metadata_t::ERROR_CODE_BAD_PACKET:
		default:
			return ERROR_UNHANDLED;
		}
	}

	// Missing timestamp
	if (!md.has_time_spec) {
		LOG(ALERT) << "UHD: Received packet missing timestamp";
		return ERROR_UNRECOVERABLE;
	}

	// Monotonicity check
	if (md.time_spec < prev_ts) {
		LOG(ALERT) << "UHD: Loss of monotonic time";
		LOG(ALERT) << "Current time: " << md.time_spec.get_real_secs() << ", "
			   << "Previous time: " << prev_ts.get_real_secs();
		return ERROR_TIMING;
	}

	// Workaround for UHD tick rounding bug
	TIMESTAMP ticks = md.time_spec.to_ticks(rx_rate);
	if (ticks - prev_ts.to_ticks(rx_rate) == rx_spp - 1)
		md.time_spec = uhd::time_spec_t::from_ticks(++ticks, rx_rate);

	prev_ts = md.time_spec;

	return 0;
}

int uhd_device::readSamples(std::vector<short *> &bufs, int len, bool *overrun,
			    TIMESTAMP timestamp, bool *underrun, unsigned *RSSI)
{
	ssize_t rc;
	uhd::time_spec_t ts;
	uhd::rx_metadata_t metadata;

	if (bufs.size() != chans) {
		LOG(ALERT) << "Invalid channel combination " << bufs.size();
		return -1;
	}

	*overrun = false;
	*underrun = false;

	// Shift read time with respect to transmit clock
	timestamp += ts_offset;

	ts = uhd::time_spec_t::from_ticks(timestamp, rx_rate);
	LOG(DEBUG) << "Requested timestamp = " << ts.get_real_secs();

	// Check that timestamp is valid
	rc = rx_buffers[0]->avail_smpls(timestamp);
	if (rc < 0) {
		LOG(ERR) << rx_buffers[0]->str_code(rc);
		LOG(ERR) << rx_buffers[0]->str_status(timestamp);
		return 0;
	}

	// Create vector buffer
	std::vector<std::vector<short> >
		pkt_bufs(chans, std::vector<short>(2 * rx_spp));

	std::vector<short *> pkt_ptrs;
	for (size_t i = 0; i < pkt_bufs.size(); i++)
		pkt_ptrs.push_back(&pkt_bufs[i].front());

	// Receive samples from the usrp until we have enough
	while (rx_buffers[0]->avail_smpls(timestamp) < len) {
		thread_enable_cancel(false);
		size_t num_smpls = rx_stream->recv(pkt_ptrs, rx_spp,
						   metadata, 0.1, true);
		thread_enable_cancel(true);

		rx_pkt_cnt++;

		// Check for errors
		rc = check_rx_md_err(metadata, num_smpls);
		switch (rc) {
		case ERROR_UNRECOVERABLE:
			LOG(ALERT) << "UHD: Version " << uhd::get_version_string();
			LOG(ALERT) << "UHD: Unrecoverable error, exiting...";
			exit(-1);
		case ERROR_TIMEOUT:
			// Assume stopping condition
			return 0;
		case ERROR_TIMING:
			restart();
		case ERROR_UNHANDLED:
			continue;
		}

		ts = metadata.time_spec;
		LOG(DEBUG) << "Received timestamp = " << ts.get_real_secs();

		for (size_t i = 0; i < rx_buffers.size(); i++) {
			rc = rx_buffers[i]->write((short *) &pkt_bufs[i].front(),
						  num_smpls,
						  metadata.time_spec);

			// Continue on local overrun, exit on other errors
			if ((rc < 0)) {
				LOG(ERR) << rx_buffers[i]->str_code(rc);
				LOG(ERR) << rx_buffers[i]->str_status(timestamp);
				if (rc != smpl_buf::ERROR_OVERFLOW)
					return 0;
			}
		}
	}

	// We have enough samples
	for (size_t i = 0; i < rx_buffers.size(); i++) {
		rc = rx_buffers[i]->read(bufs[i], len, timestamp);
		if ((rc < 0) || (rc != len)) {
			LOG(ERR) << rx_buffers[i]->str_code(rc);
			LOG(ERR) << rx_buffers[i]->str_status(timestamp);
			return 0;
		}
	}

	return len;
}

int uhd_device::writeSamples(std::vector<short *> &bufs, int len, bool *underrun,
			     unsigned long long timestamp,bool isControl)
{
	uhd::tx_metadata_t metadata;
	metadata.has_time_spec = true;
	metadata.start_of_burst = false;
	metadata.end_of_burst = false;
	metadata.time_spec = uhd::time_spec_t::from_ticks(timestamp, tx_rate);

	*underrun = false;

	// No control packets
	if (isControl) {
		LOG(ERR) << "Control packets not supported";
		return 0;
	}

	if (bufs.size() != chans) {
		LOG(ALERT) << "Invalid channel combination " << bufs.size();
		return -1;
	}

	// Drop a fixed number of packets (magic value)
	if (!aligned) {
		drop_cnt++;

		if (drop_cnt == 1) {
			LOG(DEBUG) << "Aligning transmitter: stop burst";
			*underrun = true;
			metadata.end_of_burst = true;
		} else if (drop_cnt < 30) {
			LOG(DEBUG) << "Aligning transmitter: packet advance";
			return len;
		} else {
			LOG(DEBUG) << "Aligning transmitter: start burst";
			metadata.start_of_burst = true;
			aligned = true;
			drop_cnt = 0;
		}
	}

	thread_enable_cancel(false);
	size_t num_smpls = tx_stream->send(bufs, len, metadata);
	thread_enable_cancel(true);

	if (num_smpls != (unsigned) len) {
		LOG(ALERT) << "UHD: Device send timed out";
	}

	return num_smpls;
}

bool uhd_device::updateAlignment(TIMESTAMP timestamp)
{
	return true;
}

uhd::tune_request_t uhd_device::select_freq(double freq, size_t chan, bool tx)
{
	double rf_spread, rf_freq;
	std::vector<double> freqs;
	uhd::tune_request_t treq(freq);

	if (dev_type == UMTRX) {
		if (offset != 0.0)
			return uhd::tune_request_t(freq, offset);

		// Don't use DSP tuning, because LMS6002D PLL steps are small enough.
		// We end up with DSP tuning just for 2-3Hz, which is meaningless and
		// only distort the signal (because cordic is not ideal).
		treq.target_freq = freq;
		treq.rf_freq_policy = uhd::tune_request_t::POLICY_MANUAL;
		treq.rf_freq = freq;
		treq.dsp_freq_policy = uhd::tune_request_t::POLICY_MANUAL;
		treq.dsp_freq = 0.0;
		return treq;
	} else if (chans == 1) {
		if (offset == 0.0)
			return treq;

		return uhd::tune_request_t(freq, offset);
	} else if ((dev_type != B210) || (chans > 2) || (chan > 1)) {
		LOG(ALERT) << chans << " channels unsupported";
		return treq;
	}

	if (tx)
		freqs = tx_freqs;
	else
		freqs = rx_freqs;

	/* Tune directly if other channel isn't tuned */
	if (freqs[!chan] < 10.0)
		return treq;

	/* Find center frequency between channels */
	rf_spread = fabs(freqs[!chan] - freq);
	if (rf_spread > dev_param_map.at(dev_key(B210, tx_sps, rx_sps)).mcr) {
		LOG(ALERT) << rf_spread << "Hz tuning spread not supported\n";
		return treq;
	}

	rf_freq = (freqs[!chan] + freq) / 2.0f;

	treq.rf_freq_policy = uhd::tune_request_t::POLICY_MANUAL;
	treq.target_freq = freq;
	treq.rf_freq = rf_freq;

	return treq;
}

bool uhd_device::set_freq(double freq, size_t chan, bool tx)
{
	std::vector<double> freqs;
	uhd::tune_result_t tres;
	uhd::tune_request_t treq = select_freq(freq, chan, tx);

	if (tx) {
		tres = usrp_dev->set_tx_freq(treq, chan);
		tx_freqs[chan] = usrp_dev->get_tx_freq(chan);
	} else {
		tres = usrp_dev->set_rx_freq(treq, chan);
		rx_freqs[chan] = usrp_dev->get_rx_freq(chan);
	}
	LOG(INFO) << "\n" << tres.to_pp_string() << std::endl;

	if ((chans == 1) || ((chans == 2) && dev_type == UMTRX))
		return true;

	/* Manual RF policy means we intentionally tuned with a baseband
	 * offset for dual-channel purposes. Now retune the other channel
	 * with the opposite corresponding frequency offset
	 */
	if (treq.rf_freq_policy == uhd::tune_request_t::POLICY_MANUAL) {
		if (tx) {
			treq = select_freq(tx_freqs[!chan], !chan, true);
			tres = usrp_dev->set_tx_freq(treq, !chan);
			tx_freqs[!chan] = usrp_dev->get_tx_freq(!chan);
		} else {
			treq = select_freq(rx_freqs[!chan], !chan, false);
			tres = usrp_dev->set_rx_freq(treq, !chan);
			rx_freqs[!chan] = usrp_dev->get_rx_freq(!chan);

		}
		LOG(INFO) << "\n" << tres.to_pp_string() << std::endl;
	}

	return true;
}

bool uhd_device::setTxFreq(double wFreq, size_t chan)
{
	if (chan >= tx_freqs.size()) {
		LOG(ALERT) << "Requested non-existent channel " << chan;
		return false;
	}
	ScopedLock lock(tune_lock);

	return set_freq(wFreq, chan, true);
}

bool uhd_device::setRxFreq(double wFreq, size_t chan)
{
	if (chan >= rx_freqs.size()) {
		LOG(ALERT) << "Requested non-existent channel " << chan;
		return false;
	}
	ScopedLock lock(tune_lock);

	return set_freq(wFreq, chan, false);
}

double uhd_device::getTxFreq(size_t chan)
{
	if (chan >= tx_freqs.size()) {
		LOG(ALERT) << "Requested non-existent channel " << chan;
		return 0.0;
	}

	return tx_freqs[chan];
}

double uhd_device::getRxFreq(size_t chan)
{
	if (chan >= rx_freqs.size()) {
		LOG(ALERT) << "Requested non-existent channel " << chan;
		return 0.0;
	}

	return rx_freqs[chan];
}

bool uhd_device::setRxAntenna(const std::string &ant, size_t chan)
{
	std::vector<std::string> avail;
	if (chan >= rx_paths.size()) {
		LOG(ALERT) << "Requested non-existent channel " << chan;
		return false;
	}

	avail = usrp_dev->get_rx_antennas(chan);
	if (std::find(avail.begin(), avail.end(), ant) == avail.end()) {
		LOG(ALERT) << "Requested non-existent Rx antenna " << ant << " on channel " << chan;
		LOG(INFO) << "Available Rx antennas: ";
		for (std::vector<std::string>::const_iterator i = avail.begin(); i != avail.end(); ++i)
			LOG(INFO) << "- '" << *i << "'";
		return false;
	}
	usrp_dev->set_rx_antenna(ant, chan);
	rx_paths[chan] = usrp_dev->get_rx_antenna(chan);

	if (ant != rx_paths[chan]) {
		LOG(ALERT) << "Failed setting antenna " << ant << " on channel " << chan << ", got instead " << rx_paths[chan];
		return false;
	}

	return true;
}

std::string uhd_device::getRxAntenna(size_t chan)
{
	if (chan >= rx_paths.size()) {
		LOG(ALERT) << "Requested non-existent channel " << chan;
		return "";
	}
	return usrp_dev->get_rx_antenna(chan);
}

bool uhd_device::setTxAntenna(const std::string &ant, size_t chan)
{
	std::vector<std::string> avail;
	if (chan >= tx_paths.size()) {
		LOG(ALERT) << "Requested non-existent channel " << chan;
		return false;
	}

	avail = usrp_dev->get_tx_antennas(chan);
	if (std::find(avail.begin(), avail.end(), ant) == avail.end()) {
		LOG(ALERT) << "Requested non-existent Tx antenna " << ant << " on channel " << chan;
		LOG(INFO) << "Available Tx antennas: ";
		for (std::vector<std::string>::const_iterator i = avail.begin(); i != avail.end(); ++i)
			LOG(INFO) << "- '" << *i << "'";
		return false;
	}
	usrp_dev->set_tx_antenna(ant, chan);
	tx_paths[chan] = usrp_dev->get_tx_antenna(chan);

	if (ant != tx_paths[chan]) {
		LOG(ALERT) << "Failed setting antenna " << ant << " on channel " << chan << ", got instead " << tx_paths[chan];
		return false;
	}

	return true;
}

std::string uhd_device::getTxAntenna(size_t chan)
{
	if (chan >= tx_paths.size()) {
		LOG(ALERT) << "Requested non-existent channel " << chan;
		return "";
	}
	return usrp_dev->get_tx_antenna(chan);
}

/*
 * Only allow sampling the Rx path lower than Tx and not vice-versa.
 * Using Tx with 4 SPS and Rx at 1 SPS is the only allowed mixed
 * combination.
 */
TIMESTAMP uhd_device::initialWriteTimestamp()
{
	if ((iface == MULTI_ARFCN) || (rx_sps == tx_sps))
		return ts_initial;
	else
		return ts_initial * tx_sps;
}

TIMESTAMP uhd_device::initialReadTimestamp()
{
	return ts_initial;
}

double uhd_device::fullScaleInputValue()
{
	if (dev_type == LIMESDR)
		return (double) SHRT_MAX * LIMESDR_TX_AMPL;
	if (dev_type == UMTRX)
		return (double) SHRT_MAX * UMTRX_TX_AMPL;
	else
		return (double) SHRT_MAX * USRP_TX_AMPL;
}

double uhd_device::fullScaleOutputValue()
{
	return (double) SHRT_MAX;
}

bool uhd_device::recv_async_msg()
{
	uhd::async_metadata_t md;

	thread_enable_cancel(false);
	bool rc = usrp_dev->get_device()->recv_async_msg(md);
	thread_enable_cancel(true);
	if (!rc)
		return false;

	// Assume that any error requires resynchronization
	if (md.event_code != uhd::async_metadata_t::EVENT_CODE_BURST_ACK) {
		aligned = false;

		if ((md.event_code != uhd::async_metadata_t::EVENT_CODE_UNDERFLOW) &&
		    (md.event_code != uhd::async_metadata_t::EVENT_CODE_TIME_ERROR)) {
			LOG(ERR) << str_code(md);
		}
	}

	return true;
}

std::string uhd_device::str_code(uhd::rx_metadata_t metadata)
{
	std::ostringstream ost("UHD: ");

	switch (metadata.error_code) {
	case uhd::rx_metadata_t::ERROR_CODE_NONE:
		ost << "No error";
		break;
	case uhd::rx_metadata_t::ERROR_CODE_TIMEOUT:
		ost << "No packet received, implementation timed-out";
		break;
	case uhd::rx_metadata_t::ERROR_CODE_LATE_COMMAND:
		ost << "A stream command was issued in the past";
		break;
	case uhd::rx_metadata_t::ERROR_CODE_BROKEN_CHAIN:
		ost << "Expected another stream command";
		break;
	case uhd::rx_metadata_t::ERROR_CODE_OVERFLOW:
		ost << "An internal receive buffer has filled";
		break;
	case uhd::rx_metadata_t::ERROR_CODE_ALIGNMENT:
		ost << "Multi-channel alignment failed";
		break;
	case uhd::rx_metadata_t::ERROR_CODE_BAD_PACKET:
		ost << "The packet could not be parsed";
		break;
	default:
		ost << "Unknown error " << metadata.error_code;
	}

	if (metadata.has_time_spec)
		ost << " at " << metadata.time_spec.get_real_secs() << " sec.";

	return ost.str();
}

std::string uhd_device::str_code(uhd::async_metadata_t metadata)
{
	std::ostringstream ost("UHD: ");

	switch (metadata.event_code) {
	case uhd::async_metadata_t::EVENT_CODE_BURST_ACK:
		ost << "A packet was successfully transmitted";
		break;
	case uhd::async_metadata_t::EVENT_CODE_UNDERFLOW:
		ost << "An internal send buffer has emptied";
		break;
	case uhd::async_metadata_t::EVENT_CODE_SEQ_ERROR:
		ost << "Packet loss between host and device";
		break;
	case uhd::async_metadata_t::EVENT_CODE_TIME_ERROR:
		ost << "Packet time was too late or too early";
		break;
	case uhd::async_metadata_t::EVENT_CODE_UNDERFLOW_IN_PACKET:
		ost << "Underflow occurred inside a packet";
		break;
	case uhd::async_metadata_t::EVENT_CODE_SEQ_ERROR_IN_BURST:
		ost << "Packet loss within a burst";
		break;
	default:
		ost << "Unknown error " << metadata.event_code;
	}

	if (metadata.has_time_spec)
		ost << " at " << metadata.time_spec.get_real_secs() << " sec.";

	return ost.str();
}

smpl_buf::smpl_buf(size_t len, double rate)
	: buf_len(len), clk_rt(rate),
	  time_start(0), time_end(0), data_start(0), data_end(0)
{
	data = new uint32_t[len];
}

smpl_buf::~smpl_buf()
{
	delete[] data;
}

ssize_t smpl_buf::avail_smpls(TIMESTAMP timestamp) const
{
	if (timestamp < time_start)
		return ERROR_TIMESTAMP;
	else if (timestamp >= time_end)
		return 0;
	else
		return time_end - timestamp;
}

ssize_t smpl_buf::avail_smpls(uhd::time_spec_t timespec) const
{
	return avail_smpls(timespec.to_ticks(clk_rt));
}

ssize_t smpl_buf::read(void *buf, size_t len, TIMESTAMP timestamp)
{
	int type_sz = 2 * sizeof(short);

	// Check for valid read
	if (timestamp < time_start)
		return ERROR_TIMESTAMP;
	if (timestamp >= time_end)
		return 0;
	if (len >= buf_len)
		return ERROR_READ;

	// How many samples should be copied
	size_t num_smpls = time_end - timestamp;
	if (num_smpls > len)
		num_smpls = len;

	// Starting index
	size_t read_start = (data_start + (timestamp - time_start)) % buf_len;

	// Read it
	if (read_start + num_smpls < buf_len) {
		size_t numBytes = len * type_sz;
		memcpy(buf, data + read_start, numBytes);
	} else {
		size_t first_cp = (buf_len - read_start) * type_sz;
		size_t second_cp = len * type_sz - first_cp;

		memcpy(buf, data + read_start, first_cp);
		memcpy((char*) buf + first_cp, data, second_cp);
	}

	data_start = (read_start + len) % buf_len;
	time_start = timestamp + len;

	if (time_start > time_end)
		return ERROR_READ;
	else
		return num_smpls;
}

ssize_t smpl_buf::read(void *buf, size_t len, uhd::time_spec_t ts)
{
	return read(buf, len, ts.to_ticks(clk_rt));
}

ssize_t smpl_buf::write(void *buf, size_t len, TIMESTAMP timestamp)
{
	int type_sz = 2 * sizeof(short);

	// Check for valid write
	if ((len == 0) || (len >= buf_len))
		return ERROR_WRITE;
	if ((timestamp + len) <= time_end)
		return ERROR_TIMESTAMP;

	if (timestamp < time_end) {
		LOG(ERR) << "Overwriting old buffer data: timestamp="<<timestamp<<" time_end="<<time_end;
		uhd::time_spec_t ts = uhd::time_spec_t::from_ticks(timestamp, clk_rt);
		LOG(DEBUG) << "Requested timestamp = " << timestamp << " (real_sec=" << std::fixed << ts.get_real_secs() << " = " << ts.to_ticks(clk_rt) << ") rate=" << clk_rt;
		// Do not return error here, because it's a rounding error and is not fatal
	}
	if (timestamp > time_end && time_end != 0) {
		LOG(ERR) << "Skipping buffer data: timestamp="<<timestamp<<" time_end="<<time_end;
		uhd::time_spec_t ts = uhd::time_spec_t::from_ticks(timestamp, clk_rt);
		LOG(DEBUG) << "Requested timestamp = " << timestamp << " (real_sec=" << std::fixed << ts.get_real_secs() << " = " << ts.to_ticks(clk_rt) << ") rate=" << clk_rt;
		// Do not return error here, because it's a rounding error and is not fatal
	}

	// Starting index
	size_t write_start = (data_start + (timestamp - time_start)) % buf_len;

	// Write it
	if ((write_start + len) < buf_len) {
		size_t numBytes = len * type_sz;
		memcpy(data + write_start, buf, numBytes);
	} else {
		size_t first_cp = (buf_len - write_start) * type_sz;
		size_t second_cp = len * type_sz - first_cp;

		memcpy(data + write_start, buf, first_cp);
		memcpy(data, (char*) buf + first_cp, second_cp);
	}

	data_end = (write_start + len) % buf_len;
	time_end = timestamp + len;

	if (!data_start)
		data_start = write_start;

	if (((write_start + len) > buf_len) && (data_end > data_start))
		return ERROR_OVERFLOW;
	else if (time_end <= time_start)
		return ERROR_WRITE;
	else
		return len;
}

ssize_t smpl_buf::write(void *buf, size_t len, uhd::time_spec_t ts)
{
	return write(buf, len, ts.to_ticks(clk_rt));
}

std::string smpl_buf::str_status(size_t ts) const
{
	std::ostringstream ost("Sample buffer: ");

	ost << "timestamp = " << ts;
	ost << ", length = " << buf_len;
	ost << ", time_start = " << time_start;
	ost << ", time_end = " << time_end;
	ost << ", data_start = " << data_start;
	ost << ", data_end = " << data_end;

	return ost.str();
}

std::string smpl_buf::str_code(ssize_t code)
{
	switch (code) {
	case ERROR_TIMESTAMP:
		return "Sample buffer: Requested timestamp is not valid";
	case ERROR_READ:
		return "Sample buffer: Read error";
	case ERROR_WRITE:
		return "Sample buffer: Write error";
	case ERROR_OVERFLOW:
		return "Sample buffer: Overrun";
	default:
		return "Sample buffer: Unknown error";
	}
}

RadioDevice *RadioDevice::make(size_t tx_sps, size_t rx_sps,
			       InterfaceType iface, size_t chans, double offset,
			       const std::vector<std::string>& tx_paths,
			       const std::vector<std::string>& rx_paths)
{
	return new uhd_device(tx_sps, rx_sps, iface, chans, offset, tx_paths, rx_paths);
}
