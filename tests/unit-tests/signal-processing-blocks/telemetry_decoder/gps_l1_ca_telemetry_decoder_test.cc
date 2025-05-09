/*!
 * \file gps_l1_ca_dll_pll_tracking_test.cc
 * \brief  This class implements a telemetry decoder test for GPS_L1_CA_Telemetry_Decoder
 *  implementation based on some input parameters.
 * \author Javier Arribas, 2015. jarribas(at)cttc.es
 *
 *
 * -----------------------------------------------------------------------------
 *
 * GNSS-SDR is a Global Navigation Satellite System software-defined receiver.
 * This file is part of GNSS-SDR.
 *
 * Copyright (C) 2012-2020  (see AUTHORS file for a list of contributors)
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * -----------------------------------------------------------------------------
 */

#include <armadillo>
#include <gnuradio/analog/sig_source_waveform.h>
#include <gnuradio/blocks/file_source.h>
#include <gnuradio/top_block.h>
#include <chrono>
#include <exception>
#include <iomanip>
#include <string>
#include <unistd.h>
#include <utility>
#ifdef GR_GREATER_38
#include <gnuradio/analog/sig_source.h>
#else
#include <gnuradio/analog/sig_source_c.h>
#endif
#include "GPS_L1_CA.h"
#include "gnss_block_interface.h"
#include "gnss_synchro.h"
#include "gps_l1_ca_dll_pll_tracking.h"
#include "gps_l1_ca_telemetry_decoder.h"
#include "in_memory_configuration.h"
#include "signal_generator_flags.h"
#include "telemetry_decoder_interface.h"
#include "tlm_dump_reader.h"
#include "tracking_dump_reader.h"
#include "tracking_interface.h"
#include "tracking_true_obs_reader.h"
#include <gnuradio/blocks/interleaved_char_to_complex.h>
#include <gnuradio/blocks/null_sink.h>
#include <gnuradio/blocks/skiphead.h>
#include <gtest/gtest.h>
#include <pmt/pmt.h>

#if HAS_GENERIC_LAMBDA
#else
#include <boost/bind/bind.hpp>
#endif

#if PMT_USES_BOOST_ANY
namespace wht = boost;
#else
namespace wht = std;
#endif

// ######## GNURADIO BLOCK MESSAGE RECEIVER FOR TRACKING MESSAGES #########
class GpsL1CADllPllTelemetryDecoderTest_msg_rx;

using GpsL1CADllPllTelemetryDecoderTest_msg_rx_sptr = gnss_shared_ptr<GpsL1CADllPllTelemetryDecoderTest_msg_rx>;

GpsL1CADllPllTelemetryDecoderTest_msg_rx_sptr GpsL1CADllPllTelemetryDecoderTest_msg_rx_make();

class GpsL1CADllPllTelemetryDecoderTest_msg_rx : public gr::block
{
private:
    friend GpsL1CADllPllTelemetryDecoderTest_msg_rx_sptr GpsL1CADllPllTelemetryDecoderTest_msg_rx_make();
    void msg_handler_channel_events(const pmt::pmt_t msg);
    GpsL1CADllPllTelemetryDecoderTest_msg_rx();

public:
    int rx_message;
    ~GpsL1CADllPllTelemetryDecoderTest_msg_rx();  //!< Default destructor
};

GpsL1CADllPllTelemetryDecoderTest_msg_rx_sptr GpsL1CADllPllTelemetryDecoderTest_msg_rx_make()
{
    return GpsL1CADllPllTelemetryDecoderTest_msg_rx_sptr(new GpsL1CADllPllTelemetryDecoderTest_msg_rx());
}

void GpsL1CADllPllTelemetryDecoderTest_msg_rx::msg_handler_channel_events(const pmt::pmt_t msg)
{
    try
        {
            int64_t message = pmt::to_long(std::move(msg));
            rx_message = message;
        }
    catch (const wht::bad_any_cast& e)
        {
            LOG(WARNING) << "msg_handler_channel_events Bad any_cast: " << e.what();
            rx_message = 0;
        }
}

GpsL1CADllPllTelemetryDecoderTest_msg_rx::GpsL1CADllPllTelemetryDecoderTest_msg_rx() : gr::block("GpsL1CADllPllTelemetryDecoderTest_msg_rx", gr::io_signature::make(0, 0, 0), gr::io_signature::make(0, 0, 0))
{
    this->message_port_register_in(pmt::mp("events"));
    this->set_msg_handler(pmt::mp("events"),
#if HAS_GENERIC_LAMBDA
        [this](auto&& PH1) { msg_handler_channel_events(PH1); });
#else
#if USE_BOOST_BIND_PLACEHOLDERS
        boost::bind(&GpsL1CADllPllTelemetryDecoderTest_msg_rx::msg_handler_channel_events, this, boost::placeholders::_1));
#else
        boost::bind(&GpsL1CADllPllTelemetryDecoderTest_msg_rx::msg_handler_channel_events, this, _1));
#endif
#endif
    rx_message = 0;
}

GpsL1CADllPllTelemetryDecoderTest_msg_rx::~GpsL1CADllPllTelemetryDecoderTest_msg_rx() = default;


// ###########################################################


// ######## GNURADIO BLOCK MESSAGE RECEIVER FOR TLM MESSAGES #########
class GpsL1CADllPllTelemetryDecoderTest_tlm_msg_rx;

using GpsL1CADllPllTelemetryDecoderTest_tlm_msg_rx_sptr = std::shared_ptr<GpsL1CADllPllTelemetryDecoderTest_tlm_msg_rx>;

GpsL1CADllPllTelemetryDecoderTest_tlm_msg_rx_sptr GpsL1CADllPllTelemetryDecoderTest_tlm_msg_rx_make();

class GpsL1CADllPllTelemetryDecoderTest_tlm_msg_rx : public gr::block
{
private:
    friend GpsL1CADllPllTelemetryDecoderTest_tlm_msg_rx_sptr GpsL1CADllPllTelemetryDecoderTest_tlm_msg_rx_make();
    void msg_handler_channel_events(const pmt::pmt_t msg);
    GpsL1CADllPllTelemetryDecoderTest_tlm_msg_rx();

public:
    int rx_message;
    ~GpsL1CADllPllTelemetryDecoderTest_tlm_msg_rx();  //!< Default destructor
};

GpsL1CADllPllTelemetryDecoderTest_tlm_msg_rx_sptr GpsL1CADllPllTelemetryDecoderTest_tlm_msg_rx_make()
{
    return GpsL1CADllPllTelemetryDecoderTest_tlm_msg_rx_sptr(new GpsL1CADllPllTelemetryDecoderTest_tlm_msg_rx());
}

void GpsL1CADllPllTelemetryDecoderTest_tlm_msg_rx::msg_handler_channel_events(const pmt::pmt_t msg)
{
    try
        {
            int64_t message = pmt::to_long(std::move(msg));
            rx_message = message;
        }
    catch (const wht::bad_any_cast& e)
        {
            LOG(WARNING) << "msg_handler_channel_events Bad any_cast: " << e.what();
            rx_message = 0;
        }
}

GpsL1CADllPllTelemetryDecoderTest_tlm_msg_rx::GpsL1CADllPllTelemetryDecoderTest_tlm_msg_rx() : gr::block("GpsL1CADllPllTelemetryDecoderTest_tlm_msg_rx", gr::io_signature::make(0, 0, 0), gr::io_signature::make(0, 0, 0))
{
    this->message_port_register_in(pmt::mp("events"));
    this->set_msg_handler(pmt::mp("events"),
#if HAS_GENERIC_LAMBDA
        [this](auto&& PH1) { msg_handler_channel_events(PH1); });
#else
        boost::bind(&GpsL1CADllPllTelemetryDecoderTest_tlm_msg_rx::msg_handler_channel_events, this, boost::placeholders::_1));
#endif
    rx_message = 0;
}

GpsL1CADllPllTelemetryDecoderTest_tlm_msg_rx::~GpsL1CADllPllTelemetryDecoderTest_tlm_msg_rx() = default;


// ###########################################################


class GpsL1CATelemetryDecoderTest : public ::testing::Test
{
public:
    std::string generator_binary;
    std::string p1;
    std::string p2;
    std::string p3;
    std::string p4;
    std::string p5;

#if USE_GLOG_AND_GFLAGS
    const int baseband_sampling_freq = FLAGS_fs_gen_sps;
    std::string filename_rinex_obs = FLAGS_filename_rinex_obs;
    std::string filename_raw_data = FLAGS_filename_raw_data;
#else
    const int baseband_sampling_freq = absl::GetFlag(FLAGS_fs_gen_sps);
    std::string filename_rinex_obs = absl::GetFlag(FLAGS_filename_rinex_obs);
    std::string filename_raw_data = absl::GetFlag(FLAGS_filename_raw_data);
#endif
    int configure_generator();
    int generate_signal();
    void check_results(arma::vec& true_time_s,
        arma::vec& true_value,
        arma::vec& meas_time_s,
        arma::vec& meas_value);

    GpsL1CATelemetryDecoderTest()
    {
        config = std::make_shared<InMemoryConfiguration>();
        item_size = sizeof(gr_complex);
        gnss_synchro = Gnss_Synchro();
    }

    ~GpsL1CATelemetryDecoderTest() = default;

    void configure_receiver();

    gr::top_block_sptr top_block;
    std::shared_ptr<InMemoryConfiguration> config;
    Gnss_Synchro gnss_synchro;
    size_t item_size;
};


int GpsL1CATelemetryDecoderTest::configure_generator()
{
#if USE_GLOG_AND_GFLAGS
    // Configure signal generator
    generator_binary = FLAGS_generator_binary;

    p1 = std::string("-rinex_nav_file=") + FLAGS_rinex_nav_file;
    if (FLAGS_dynamic_position.empty())
        {
            p2 = std::string("-static_position=") + FLAGS_static_position + std::string(",") + std::to_string(FLAGS_duration * 10);
        }
    else
        {
            p2 = std::string("-obs_pos_file=") + std::string(FLAGS_dynamic_position);
        }
    p3 = std::string("-rinex_obs_file=") + FLAGS_filename_rinex_obs;               // RINEX 2.10 observation file output
    p4 = std::string("-sig_out_file=") + FLAGS_filename_raw_data;                  // Baseband signal output file. Will be stored in int8_t IQ multiplexed samples
    p5 = std::string("-sampling_freq=") + std::to_string(baseband_sampling_freq);  // Baseband sampling frequency [MSps]
#else
    // Configure signal generator
    generator_binary = absl::GetFlag(FLAGS_generator_binary);

    p1 = std::string("-rinex_nav_file=") + absl::GetFlag(FLAGS_rinex_nav_file);
    if (absl::GetFlag(FLAGS_dynamic_position).empty())
        {
            p2 = std::string("-static_position=") + absl::GetFlag(FLAGS_static_position) + std::string(",") + std::to_string(absl::GetFlag(FLAGS_duration) * 10);
        }
    else
        {
            p2 = std::string("-obs_pos_file=") + std::string(absl::GetFlag(FLAGS_dynamic_position));
        }
    p3 = std::string("-rinex_obs_file=") + absl::GetFlag(FLAGS_filename_rinex_obs);  // RINEX 2.10 observation file output
    p4 = std::string("-sig_out_file=") + absl::GetFlag(FLAGS_filename_raw_data);     // Baseband signal output file. Will be stored in int8_t IQ multiplexed samples
    p5 = std::string("-sampling_freq=") + std::to_string(baseband_sampling_freq);    // Baseband sampling frequency [MSps]
#endif

    return 0;
}


int GpsL1CATelemetryDecoderTest::generate_signal()
{
    int child_status;

    char* const parmList[] = {&generator_binary[0], &generator_binary[0], &p1[0], &p2[0], &p3[0], &p4[0], &p5[0], nullptr};

    int pid;
    if ((pid = fork()) == -1)
        {
            perror("fork err");
        }
    else if (pid == 0)
        {
            execv(&generator_binary[0], parmList);
            std::cout << "Return not expected. Must be an execv err.\n";
            std::terminate();
        }

    waitpid(pid, &child_status, 0);

    std::cout << "Signal and Observables RINEX and RAW files created.\n";
    return 0;
}


void GpsL1CATelemetryDecoderTest::configure_receiver()
{
    gnss_synchro.Channel_ID = 0;
    gnss_synchro.System = 'G';
    std::string signal = "1C";
    signal.copy(gnss_synchro.Signal, 2, 0);
#if USE_GLOG_AND_GFLAGS
    gnss_synchro.PRN = FLAGS_test_satellite_PRN;
#else
    gnss_synchro.PRN = absl::GetFlag(FLAGS_test_satellite_PRN);
#endif
    config->set_property("GNSS-SDR.internal_fs_sps", std::to_string(baseband_sampling_freq));

    // Set Tracking
    config->set_property("Tracking_1C.item_type", "gr_complex");
    config->set_property("Tracking_1C.dump", "true");
    config->set_property("Tracking_1C.dump_filename", "./tracking_ch_");
    config->set_property("Tracking_1C.pll_bw_hz", "20.0");
    config->set_property("Tracking_1C.dll_bw_hz", "1.5");
    config->set_property("Tracking_1C.early_late_space_chips", "0.5");
    config->set_property("Tracking_1C.unified", "true");
    config->set_property("TelemetryDecoder_1C.dump", "true");
}


void GpsL1CATelemetryDecoderTest::check_results(arma::vec& true_time_s,
    arma::vec& true_value,
    arma::vec& meas_time_s,
    arma::vec& meas_value)
{
    // 1. True value interpolation to match the measurement times
    arma::vec true_value_interp;
    arma::uvec true_time_s_valid = find(true_time_s > 0);
    true_time_s = true_time_s(true_time_s_valid);
    true_value = true_value(true_time_s_valid);
    arma::uvec meas_time_s_valid = find(meas_time_s > 0);
    meas_time_s = meas_time_s(meas_time_s_valid);
    meas_value = meas_value(meas_time_s_valid);

    arma::interp1(true_time_s, true_value, meas_time_s, true_value_interp);

    // 2. RMSE
    // arma::vec err = meas_value - true_value_interp + 0.001;
    arma::vec err = meas_value - true_value_interp;  // - 0.001;

    arma::vec err2 = arma::square(err);
    double rmse = sqrt(arma::mean(err2));

    // 3. Mean err and variance
    double error_mean = arma::mean(err);
    double error_var = arma::var(err);

    // 4. Peaks
    double max_error = arma::max(err);
    double min_error = arma::min(err);

    // 5. report
    std::streamsize ss = std::cout.precision();
    std::cout << std::setprecision(10) << "TLM TOW RMSE="
              << rmse << ", mean=" << error_mean
              << ", stdev=" << sqrt(error_var)
              << " (max,min)=" << max_error
              << "," << min_error
              << " [Seconds]\n";
    std::cout.precision(ss);

    ASSERT_LT(rmse, 0.3E-6);
    ASSERT_LT(error_mean, 0.3E-6);
    ASSERT_GT(error_mean, -0.3E-6);
    ASSERT_LT(error_var, 0.3E-6);
    ASSERT_LT(max_error, 0.5E-6);
    ASSERT_GT(min_error, -0.5E-6);
}


TEST_F(GpsL1CATelemetryDecoderTest, ValidationOfResults)
{
    // Configure the signal generator
    configure_generator();

    // Generate signal raw signal samples and observations RINEX file
#if USE_GLOG_AND_GFLAGS
    if (FLAGS_disable_generator == false)
#else
    if (absl::GetFlag(FLAGS_disable_generator) == false)
#endif
        {
            generate_signal();
        }

    std::chrono::time_point<std::chrono::system_clock> start, end;
    std::chrono::duration<double> elapsed_seconds(0);

    configure_receiver();

    // open true observables log file written by the simulator
    Tracking_True_Obs_Reader true_obs_data;
#if USE_GLOG_AND_GFLAGS
    int test_satellite_PRN = FLAGS_test_satellite_PRN;
#else
    int test_satellite_PRN = absl::GetFlag(FLAGS_test_satellite_PRN);
#endif
    std::cout << "Testing satellite PRN=" << test_satellite_PRN << '\n';
    std::string true_obs_file = std::string("./gps_l1_ca_obs_prn");
    true_obs_file.append(std::to_string(test_satellite_PRN));
    true_obs_file.append(".dat");
    ASSERT_NO_THROW({
        if (true_obs_data.open_obs_file(true_obs_file) == false)
            {
                throw std::exception();
            };
    }) << "Failure opening true observables file";

    top_block = gr::make_top_block("Telemetry_Decoder test");
    std::shared_ptr<TrackingInterface> tracking = std::make_shared<GpsL1CaDllPllTracking>(config.get(), "Tracking_1C", 1, 1);
    // std::shared_ptr<TrackingInterface> tracking = std::make_shared<GpsL1CaDllPllCAidTracking>(config.get(), "Tracking_1C", 1, 1);

    auto msg_rx = GpsL1CADllPllTelemetryDecoderTest_msg_rx_make();

    // load acquisition data based on the first epoch of the true observations
    ASSERT_NO_THROW({
        if (true_obs_data.read_binary_obs() == false)
            {
                throw std::exception();
            };
    }) << "Failure reading true observables file";

    // restart the epoch counter
    true_obs_data.restart();

    std::cout << "Initial Doppler [Hz]=" << true_obs_data.doppler_l1_hz << " Initial code delay [Chips]=" << true_obs_data.prn_delay_chips << '\n';
    gnss_synchro.Acq_delay_samples = (GPS_L1_CA_CODE_LENGTH_CHIPS - true_obs_data.prn_delay_chips / GPS_L1_CA_CODE_LENGTH_CHIPS) * baseband_sampling_freq * GPS_L1_CA_CODE_PERIOD_S;
    gnss_synchro.Acq_doppler_hz = true_obs_data.doppler_l1_hz;
    gnss_synchro.Acq_samplestamp_samples = 0;

    std::shared_ptr<TelemetryDecoderInterface> tlm = std::make_shared<GpsL1CaTelemetryDecoder>(config.get(), "TelemetryDecoder_1C", 1, 1);
    tlm->set_channel(0);

    std::shared_ptr<GpsL1CADllPllTelemetryDecoderTest_tlm_msg_rx> tlm_msg_rx = GpsL1CADllPllTelemetryDecoderTest_tlm_msg_rx_make();

    ASSERT_NO_THROW({
        tracking->set_channel(gnss_synchro.Channel_ID);
    }) << "Failure setting channel.";

    ASSERT_NO_THROW({
        tracking->set_gnss_synchro(&gnss_synchro);
    }) << "Failure setting gnss_synchro.";

    ASSERT_NO_THROW({
        tracking->connect(top_block);
    }) << "Failure connecting tracking to the top_block.";

    ASSERT_NO_THROW({
        std::string file = "./" + filename_raw_data;
        const char* file_name = file.c_str();
        gr::blocks::file_source::sptr file_source = gr::blocks::file_source::make(sizeof(int8_t), file_name, false);
        gr::blocks::interleaved_char_to_complex::sptr gr_interleaved_char_to_complex = gr::blocks::interleaved_char_to_complex::make();
        gr::blocks::null_sink::sptr sink = gr::blocks::null_sink::make(sizeof(Gnss_Synchro));
        top_block->connect(file_source, 0, gr_interleaved_char_to_complex, 0);
        top_block->connect(gr_interleaved_char_to_complex, 0, tracking->get_left_block(), 0);
        top_block->connect(tracking->get_right_block(), 0, tlm->get_left_block(), 0);
        top_block->connect(tlm->get_right_block(), 0, sink, 0);
        top_block->msg_connect(tracking->get_right_block(), pmt::mp("events"), msg_rx, pmt::mp("events"));
    }) << "Failure connecting the blocks.";

    tracking->start_tracking();

    EXPECT_NO_THROW({
        start = std::chrono::system_clock::now();
        top_block->run();  // Start threads and wait
        end = std::chrono::system_clock::now();
        elapsed_seconds = end - start;
    }) << "Failure running the top_block.";

    // check results
    // load the true values
    int64_t nepoch = true_obs_data.num_epochs();
    std::cout << "True observation epochs=" << nepoch << '\n';

    arma::vec true_timestamp_s = arma::zeros(nepoch, 1);
    arma::vec true_acc_carrier_phase_cycles = arma::zeros(nepoch, 1);
    arma::vec true_Doppler_Hz = arma::zeros(nepoch, 1);
    arma::vec true_prn_delay_chips = arma::zeros(nepoch, 1);
    arma::vec true_tow_s = arma::zeros(nepoch, 1);

    int64_t epoch_counter = 0;
    while (true_obs_data.read_binary_obs())
        {
            true_timestamp_s(epoch_counter) = true_obs_data.signal_timestamp_s;
            true_acc_carrier_phase_cycles(epoch_counter) = true_obs_data.acc_carrier_phase_cycles;
            true_Doppler_Hz(epoch_counter) = true_obs_data.doppler_l1_hz;
            true_prn_delay_chips(epoch_counter) = true_obs_data.prn_delay_chips;
            true_tow_s(epoch_counter) = true_obs_data.tow;
            epoch_counter++;
        }

    // load the measured values
    Tlm_Dump_Reader tlm_dump;
    ASSERT_NO_THROW({
        if (tlm_dump.open_obs_file(std::string("./telemetry0.dat")) == false)
            {
                throw std::exception();
            };
    }) << "Failure opening telemetry dump file";

    nepoch = tlm_dump.num_epochs();
    std::cout << "Measured observation epochs=" << nepoch << '\n';

    arma::vec tlm_timestamp_s = arma::zeros(nepoch, 1);
    arma::vec tlm_TOW_at_Preamble = arma::zeros(nepoch, 1);
    arma::vec tlm_tow_s = arma::zeros(nepoch, 1);

    epoch_counter = 0;
    while (tlm_dump.read_binary_obs())
        {
            tlm_timestamp_s(epoch_counter) = static_cast<double>(tlm_dump.Tracking_sample_counter) / static_cast<double>(baseband_sampling_freq);
            tlm_TOW_at_Preamble(epoch_counter) = tlm_dump.d_TOW_at_Preamble;
            tlm_tow_s(epoch_counter) = tlm_dump.TOW_at_current_symbol;
            epoch_counter++;
        }

    // Cut measurement initial transitory of the measurements
    arma::uvec initial_meas_point = arma::find(tlm_tow_s >= true_tow_s(0), 1, "first");
    ASSERT_EQ(initial_meas_point.is_empty(), false);
    tlm_timestamp_s = tlm_timestamp_s.subvec(initial_meas_point(0), tlm_timestamp_s.size() - 1);
    tlm_tow_s = tlm_tow_s.subvec(initial_meas_point(0), tlm_tow_s.size() - 1);

    check_results(true_timestamp_s, true_tow_s, tlm_timestamp_s, tlm_tow_s);

    std::cout << "Test completed in " << elapsed_seconds.count() * 1e6 << " microseconds\n";
}
