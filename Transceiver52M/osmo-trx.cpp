/*
 * Copyright (C) 2013 Thomas Tsou <tom@tsou.cc>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "Transceiver.h"
#include "radioDevice.h"

#include <time.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <sched.h>
#include <vector>
#include <string>
#include <sstream>
#include <iostream>

#include <GSMCommon.h>
#include <Logger.h>

extern "C" {
#include <osmocom/core/talloc.h>
#include <osmocom/core/application.h>
#include <osmocom/core/msgb.h>
#include <osmocom/core/stats.h>
#include <osmocom/vty/logging.h>
#include <osmocom/vty/ports.h>
#include <osmocom/vty/misc.h>
#include <osmocom/vty/telnet_interface.h>
#include <osmocom/ctrl/control_vty.h>
#include <osmocom/ctrl/ports.h>
#include <osmocom/ctrl/control_if.h>
#include <osmocom/vty/stats.h>
#include "convolve.h"
#include "convert.h"
#include "trx_vty.h"
#include "debug.h"
}

#define DEFAULT_CONFIG_FILE	"osmo-trx.cfg"

#define charp2str(a) ((a) ? std::string(a) : std::string(""))

static char* config_file = (char*)DEFAULT_CONFIG_FILE;

volatile bool gshutdown = false;

static void *tall_trx_ctx;
static struct trx_ctx *g_trx_ctx;
static struct ctrl_handle *g_ctrlh;

static RadioDevice *usrp;
static RadioInterface *radio;
static Transceiver *transceiver;

/* Create radio interface
 *     The interface consists of sample rate changes, frequency shifts,
 *     channel multiplexing, and other conversions. The transceiver core
 *     accepts input vectors sampled at multiples of the GSM symbol rate.
 *     The radio interface connects the main transceiver with the device
 *     object, which may be operating some other rate.
 */
RadioInterface *makeRadioInterface(struct trx_ctx *trx,
                                   RadioDevice *usrp, int type)
{
	RadioInterface *radio = NULL;

	switch (type) {
	case RadioDevice::NORMAL:
		radio = new RadioInterface(usrp, trx->cfg.tx_sps,
					   trx->cfg.rx_sps, trx->cfg.num_chans);
		break;
	case RadioDevice::RESAMP_64M:
	case RadioDevice::RESAMP_100M:
		radio = new RadioInterfaceResamp(usrp, trx->cfg.tx_sps,
						 trx->cfg.rx_sps);
		break;
	case RadioDevice::MULTI_ARFCN:
		radio = new RadioInterfaceMulti(usrp, trx->cfg.tx_sps,
						trx->cfg.rx_sps, trx->cfg.num_chans);
		break;
	default:
		LOG(ALERT) << "Unsupported radio interface configuration";
		return NULL;
	}

	if (!radio->init(type)) {
		LOG(ALERT) << "Failed to initialize radio interface";
		return NULL;
	}

	return radio;
}

/* Create transceiver core
 *     The multi-threaded modem core operates at multiples of the GSM rate of
 *     270.8333 ksps and consists of GSM specific modulation, demodulation,
 *     and decoding schemes. Also included are the socket interfaces for
 *     connecting to the upper layer stack.
 */
int makeTransceiver(struct trx_ctx *trx, RadioInterface *radio)
{
	VectorFIFO *fifo;

	transceiver = new Transceiver(trx->cfg.base_port, trx->cfg.bind_addr,
			      trx->cfg.remote_addr, trx->cfg.tx_sps,
			      trx->cfg.rx_sps, trx->cfg.num_chans, GSM::Time(3,0),
			      radio, trx->cfg.rssi_offset);
	if (!transceiver->init(trx->cfg.filler, trx->cfg.rtsc,
		       trx->cfg.rach_delay, trx->cfg.egprs)) {
		LOG(ALERT) << "Failed to initialize transceiver";
		return -1;
	}

	for (size_t i = 0; i < trx->cfg.num_chans; i++) {
		fifo = radio->receiveFIFO(i);
		if (fifo && transceiver->receiveFIFO(fifo, i))
			continue;

		LOG(ALERT) << "Could not attach FIFO to channel " << i;
		return -1;
	}
	return 0;
}

static void sig_handler(int signo)
{
	fprintf(stdout, "signal %d received\n", signo);
	switch (signo) {
	case SIGINT:
	case SIGTERM:
		fprintf(stdout, "shutting down\n");
		gshutdown = true;
		break;
	case SIGABRT:
	case SIGUSR1:
		talloc_report(tall_trx_ctx, stderr);
		talloc_report_full(tall_trx_ctx, stderr);
		break;
	case SIGUSR2:
		talloc_report_full(tall_trx_ctx, stderr);
		break;
	default:
		break;
	}
}

static void setup_signal_handlers()
{
	/* Handle keyboard interrupt SIGINT */
	signal(SIGINT, &sig_handler);
	signal(SIGTERM, &sig_handler);
	signal(SIGABRT, &sig_handler);
	signal(SIGUSR1, &sig_handler);
	signal(SIGUSR2, &sig_handler);
	osmo_init_ignore_signals();
}

static std::vector<std::string> comma_delimited_to_vector(char* opt)
{
	std::string str = std::string(opt);
	std::vector<std::string> result;
	std::stringstream ss(str);

	while( ss.good() )
	{
	    std::string substr;
	    getline(ss, substr, ',');
	    result.push_back(substr);
	}
	return result;
}

static void print_help()
{
	fprintf(stdout, "Options:\n"
		"  -h    This text\n"
		"  -C    Filename The config file to use\n"
		);
}

static void print_deprecated(char opt)
{
	LOG(WARNING) << "Cmd line option '" << opt << "' is deprecated and will be soon removed."
		<< " Please use VTY cfg option instead."
		<< " All cmd line options are already being overriden by VTY options if set.";
}

static void handle_options(int argc, char **argv, struct trx_ctx* trx)
{
	int option;
	unsigned int i;
	std::vector<std::string> rx_paths, tx_paths;
	bool rx_paths_set = false, tx_paths_set = false;

	while ((option = getopt(argc, argv, "ha:l:i:j:p:c:dmxgfo:s:b:r:A:R:Set:y:z:C:")) != -1) {
		switch (option) {
		case 'h':
			print_help();
			exit(0);
			break;
		case 'a':
			print_deprecated(option);
			osmo_talloc_replace_string(trx, &trx->cfg.dev_args, optarg);
			break;
		case 'l':
			print_deprecated(option);
			log_set_log_level(osmo_stderr_target, atoi(optarg));
			break;
		case 'i':
			print_deprecated(option);
			osmo_talloc_replace_string(trx, &trx->cfg.remote_addr, optarg);
			break;
		case 'j':
			print_deprecated(option);
			osmo_talloc_replace_string(trx, &trx->cfg.bind_addr, optarg);
			break;
		case 'p':
			print_deprecated(option);
			trx->cfg.base_port = atoi(optarg);
			break;
		case 'c':
			print_deprecated(option);
			trx->cfg.num_chans = atoi(optarg);
			break;
		case 'm':
			print_deprecated(option);
			trx->cfg.multi_arfcn = true;
			break;
		case 'x':
			print_deprecated(option);
			trx->cfg.clock_ref = REF_EXTERNAL;
			break;
		case 'g':
			print_deprecated(option);
			trx->cfg.clock_ref = REF_GPS;
			break;
		case 'f':
			print_deprecated(option);
			trx->cfg.filler = FILLER_DUMMY;
			break;
		case 'o':
			print_deprecated(option);
			trx->cfg.offset = atof(optarg);
			break;
		case 's':
			print_deprecated(option);
			trx->cfg.tx_sps = atoi(optarg);
			break;
		case 'b':
			print_deprecated(option);
			trx->cfg.rx_sps = atoi(optarg);
			break;
		case 'r':
			print_deprecated(option);
			trx->cfg.rtsc_set = true;
			trx->cfg.rtsc = atoi(optarg);
			if (!trx->cfg.egprs) /* Don't override egprs which sets different filler */
				trx->cfg.filler = FILLER_NORM_RAND;
			break;
		case 'A':
			print_deprecated(option);
			trx->cfg.rach_delay_set = true;
			trx->cfg.rach_delay = atoi(optarg);
			trx->cfg.filler = FILLER_ACCESS_RAND;
			break;
		case 'R':
			print_deprecated(option);
			trx->cfg.rssi_offset = atof(optarg);
			break;
		case 'S':
			print_deprecated(option);
			trx->cfg.swap_channels = true;
			break;
		case 'e':
			print_deprecated(option);
			trx->cfg.egprs = true;
			break;
		case 't':
			print_deprecated(option);
			trx->cfg.sched_rr = atoi(optarg);
			break;
		case 'y':
			print_deprecated(option);
			tx_paths = comma_delimited_to_vector(optarg);
			tx_paths_set = true;
			break;
		case 'z':
			print_deprecated(option);
			rx_paths = comma_delimited_to_vector(optarg);
			rx_paths_set = true;
			break;
		case 'C':
			config_file = optarg;
			break;
		default:
			goto bad_config;
		}
	}

	/* Cmd line option specific validation & setup */

	if (trx->cfg.num_chans > TRX_CHAN_MAX) {
		LOG(ERROR) << "Too many channels requested, maximum is " <<  TRX_CHAN_MAX;
		goto bad_config;
	}
	if ((tx_paths_set && tx_paths.size() != trx->cfg.num_chans) ||
	    (rx_paths_set && rx_paths.size() != trx->cfg.num_chans)) {
		LOG(ERROR) << "Num of channels and num of Rx/Tx Antennas doesn't match";
		goto bad_config;
	}
	for (i = 0; i < trx->cfg.num_chans; i++) {
		trx->cfg.chans[i].trx = trx;
		trx->cfg.chans[i].idx = i;
		if (tx_paths_set)
			osmo_talloc_replace_string(trx, &trx->cfg.chans[i].tx_path, tx_paths[i].c_str());
		if (rx_paths_set)
			osmo_talloc_replace_string(trx, &trx->cfg.chans[i].rx_path, rx_paths[i].c_str());
	}

	return;

bad_config:
	print_help();
	exit(0);
}

int trx_validate_config(struct trx_ctx *trx)
{
	if (trx->cfg.multi_arfcn && trx->cfg.num_chans > 5) {
		LOG(ERROR) << "Unsupported number of channels";
		return -1;
	}

	/* Force 4 SPS for EDGE or multi-ARFCN configurations */
	if ((trx->cfg.egprs || trx->cfg.multi_arfcn) &&
	    (trx->cfg.tx_sps!=4 || trx->cfg.tx_sps!=4)) {
		LOG(ERROR) << "EDGE and Multi-Carrier options require 4 tx and rx sps. Check you config.";
		return -1;
	}

	return 0;
}

static int set_sched_rr(unsigned int prio)
{
	struct sched_param param;
	int rc;
	memset(&param, 0, sizeof(param));
	param.sched_priority = prio;
	printf("Setting SCHED_RR priority(%d)\n", param.sched_priority);
	rc = sched_setscheduler(getpid(), SCHED_RR, &param);
	if (rc != 0) {
		LOG(ERROR) << "Config: Setting SCHED_RR failed";
		return -1;
	}
	return 0;
}

static void print_config(struct trx_ctx *trx)
{
	unsigned int i;
	std::ostringstream ost("");

	ost << "Config Settings" << std::endl;
	ost << "   Log Level............... " << (unsigned int) osmo_stderr_target->loglevel << std::endl;
	ost << "   Device args............. " << charp2str(trx->cfg.dev_args) << std::endl;
	ost << "   TRX Base Port........... " << trx->cfg.base_port << std::endl;
	ost << "   TRX Address............. " << charp2str(trx->cfg.bind_addr) << std::endl;
	ost << "   GSM Core Address........." << charp2str(trx->cfg.remote_addr) << std::endl;
	ost << "   Channels................ " << trx->cfg.num_chans << std::endl;
	ost << "   Tx Samples-per-Symbol... " << trx->cfg.tx_sps << std::endl;
	ost << "   Rx Samples-per-Symbol... " << trx->cfg.rx_sps << std::endl;
	ost << "   EDGE support............ " << trx->cfg.egprs << std::endl;
	ost << "   Reference............... " << trx->cfg.clock_ref << std::endl;
	ost << "   C0 Filler Table......... " << trx->cfg.filler << std::endl;
	ost << "   Multi-Carrier........... " << trx->cfg.multi_arfcn << std::endl;
	ost << "   Tuning offset........... " << trx->cfg.offset << std::endl;
	ost << "   RSSI to dBm offset...... " << trx->cfg.rssi_offset << std::endl;
	ost << "   Swap channels........... " << trx->cfg.swap_channels << std::endl;
	ost << "   Tx Antennas.............";
	for (i = 0; i < trx->cfg.num_chans; i++) {
		std::string p = charp2str(trx->cfg.chans[i].tx_path);
		ost << " '" << ((p != "") ? p : "<default>") <<  "'";
	}
	ost << std::endl;
	ost << "   Rx Antennas.............";
	for (i = 0; i < trx->cfg.num_chans; i++) {
		std::string p = charp2str(trx->cfg.chans[i].rx_path);
		ost << " '" << ((p != "") ? p : "<default>") <<  "'";
	}
	ost << std::endl;

	std::cout << ost << std::endl;
}

static void trx_stop()
{
	std::cout << "Shutting down transceiver..." << std::endl;

	delete transceiver;
	delete radio;
	delete usrp;
}

static int trx_start(struct trx_ctx *trx)
{
	int type, chans;
	unsigned int i;
	std::vector<std::string> rx_paths, tx_paths;
	RadioDevice::InterfaceType iface = RadioDevice::NORMAL;

	/* Create the low level device object */
	if (trx->cfg.multi_arfcn)
		iface = RadioDevice::MULTI_ARFCN;

	/* Generate vector of rx/tx_path: */
	for (i = 0; i < trx->cfg.num_chans; i++) {
		rx_paths.push_back(charp2str(trx->cfg.chans[i].rx_path));
		tx_paths.push_back(charp2str(trx->cfg.chans[i].tx_path));
	}

	usrp = RadioDevice::make(trx->cfg.tx_sps, trx->cfg.rx_sps, iface,
				 trx->cfg.num_chans, trx->cfg.offset,
				 tx_paths, rx_paths);
	type = usrp->open(charp2str(trx->cfg.dev_args), trx->cfg.clock_ref, trx->cfg.swap_channels);
	if (type < 0) {
		LOG(ALERT) << "Failed to create radio device" << std::endl;
		goto shutdown;
	}

	/* Setup the appropriate device interface */
	radio = makeRadioInterface(trx, usrp, type);
	if (!radio)
		goto shutdown;

	/* Create the transceiver core */
	if (makeTransceiver(trx, radio) < 0)
		goto shutdown;

	chans = transceiver->numChans();
	std::cout << "-- Transceiver active with "
		  << chans << " channel(s)" << std::endl;

	return 0;

shutdown:
	trx_stop();
	return -1;
}

int main(int argc, char *argv[])
{
	int rc;

	tall_trx_ctx = talloc_named_const(NULL, 0, "OsmoTRX");
	msgb_talloc_ctx_init(tall_trx_ctx, 0);
	g_vty_info.tall_ctx = tall_trx_ctx;

	setup_signal_handlers();

	g_trx_ctx = vty_trx_ctx_alloc(tall_trx_ctx);

#ifdef HAVE_SSE3
	printf("Info: SSE3 support compiled in");
#ifdef HAVE___BUILTIN_CPU_SUPPORTS
	if (__builtin_cpu_supports("sse3"))
		printf(" and supported by CPU\n");
	else
		printf(", but not supported by CPU\n");
#else
	printf(", but runtime SIMD detection disabled\n");
#endif
#endif

#ifdef HAVE_SSE4_1
	printf("Info: SSE4.1 support compiled in");
#ifdef HAVE___BUILTIN_CPU_SUPPORTS
	if (__builtin_cpu_supports("sse4.1"))
		printf(" and supported by CPU\n");
	else
		printf(", but not supported by CPU\n");
#else
	printf(", but runtime SIMD detection disabled\n");
#endif
#endif

	convolve_init();
	convert_init();

	osmo_init_logging(&log_info);
	osmo_stats_init(tall_trx_ctx);
	vty_init(&g_vty_info);
	ctrl_vty_init(tall_trx_ctx);
	trx_vty_init(g_trx_ctx);

	logging_vty_add_cmds();
	osmo_talloc_vty_add_cmds();
	osmo_stats_vty_add_cmds();

	handle_options(argc, argv, g_trx_ctx);

	rate_ctr_init(tall_trx_ctx);

	rc = vty_read_config_file(config_file, NULL);
	if (rc < 0) {
		fprintf(stderr, "Failed to open config file: '%s'\n", config_file);
		exit(2);
	}

	rc = telnet_init_dynif(tall_trx_ctx, NULL, vty_get_bind_addr(), OSMO_VTY_PORT_TRX);
	if (rc < 0)
		exit(1);

	g_ctrlh = ctrl_interface_setup(NULL, OSMO_CTRL_PORT_TRX, NULL);
	if (!g_ctrlh) {
		fprintf(stderr, "Failed to create CTRL interface.\n");
		exit(1);
	}

	/* Backward compatibility: Hack to have 1 channel allocated by default.
	 * Can be Dropped once we * get rid of "-c" cmdline param */
	if (g_trx_ctx->cfg.num_chans == 0) {
		g_trx_ctx->cfg.num_chans = 1;
		g_trx_ctx->cfg.chans[0].trx = g_trx_ctx;
		g_trx_ctx->cfg.chans[0].idx = 0;
		LOG(ERROR) << "No explicit channel config found. Make sure you" \
			" configure channels in VTY config. Using 1 channel as default," \
			" but expect your config to break in the future.";
	}

	print_config(g_trx_ctx);

	if (trx_validate_config(g_trx_ctx) < 0) {
		LOG(ERROR) << "Config failure - exiting";
		return EXIT_FAILURE;
	}

	if (g_trx_ctx->cfg.sched_rr) {
		if (set_sched_rr(g_trx_ctx->cfg.sched_rr) < 0)
			return EXIT_FAILURE;
	}

	srandom(time(NULL));

	if(trx_start(g_trx_ctx) < 0)
		return EXIT_FAILURE;

	while (!gshutdown)
		osmo_select_main(0);

	trx_stop();

	return 0;
}
