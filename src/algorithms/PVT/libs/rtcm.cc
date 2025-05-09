/*!
 * \file rtcm.cc
 * \brief  Implementation of RTCM 3.2 Standard
 * \author Carles Fernandez-Prades, 2015. cfernandez(at)cttc.es
 *
 * -----------------------------------------------------------------------------
 *
 * GNSS-SDR is a Global Navigation Satellite System software-defined receiver.
 * This file is part of GNSS-SDR.
 *
 * Copyright (C) 2010-2020  (see AUTHORS file for a list of contributors)
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * -----------------------------------------------------------------------------
 */

#include "rtcm.h"
#include "GLONASS_L1_L2_CA.h"
#include "GPS_L1_CA.h"
#include "GPS_L2C.h"
#include "Galileo_E1.h"
#include "Galileo_E5a.h"
#include "Galileo_E5b.h"
#include "Galileo_FNAV.h"
#include "Galileo_INAV.h"
#include <boost/algorithm/string.hpp>  // for to_upper_copy
#include <boost/crc.hpp>
#include <boost/date_time/gregorian/gregorian.hpp>
#include <boost/dynamic_bitset.hpp>
#include <boost/exception/diagnostic_information.hpp>
#include <algorithm>  // for std::reverse
#include <cmath>      // for std::fmod, std::lround
#include <cstdlib>    // for strtol
#include <iostream>   // for std::cout


Rtcm::Rtcm(uint16_t port) : RTCM_port(port), server_is_running(false)
{
    preamble = std::bitset<8>("11010011");
    reserved_field = std::bitset<6>("000000");
    rtcm_message_queue = std::make_shared<Concurrent_Queue<std::string>>();
    boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::tcp::v4(), RTCM_port);
    servers.emplace_back(io_context, endpoint);
}


Rtcm::~Rtcm()
{
    DLOG(INFO) << "RTCM object destructor called.";
    if (server_is_running)
        {
            try
                {
                    stop_server();
                }
            catch (const boost::exception& e)
                {
                    LOG(WARNING) << "Boost exception: " << boost::diagnostic_information(e);
                }
            catch (const std::exception& ex)
                {
                    LOG(WARNING) << "STD exception: " << ex.what();
                }
        }
}


// *****************************************************************************************************
//
//   TCP Server helper classes
//
// *****************************************************************************************************
void Rtcm::run_server()
{
    std::cout << "Starting a TCP/IP server of RTCM messages on port " << RTCM_port << '\n';
    try
        {
            tq = std::thread([&] { std::make_shared<Queue_Reader>(io_context, rtcm_message_queue, RTCM_port)->do_read_queue(); });
            t = std::thread([&] { io_context.run(); });
            server_is_running = true;
        }
    catch (const std::exception& e)
        {
            std::cerr << "Exception: " << e.what() << "\n";
        }
}


void Rtcm::stop_service()
{
    io_context.stop();
}


void Rtcm::stop_server()
{
    std::cout << "Stopping TCP/IP server on port " << RTCM_port << '\n';
    Rtcm::stop_service();
    servers.front().close_server();
    rtcm_message_queue->push("Goodbye");  // this terminates tq
    tq.join();
    t.join();
    server_is_running = false;
}


void Rtcm::send_message(const std::string& msg)
{
    rtcm_message_queue->push(msg);
}


bool Rtcm::is_server_running() const
{
    return server_is_running;
}


// *****************************************************************************************************
//
//   TRANSPORT LAYER AS DEFINED AT RTCM STANDARD 10403.2
//
// *****************************************************************************************************

std::string Rtcm::add_CRC(const std::string& message_without_crc) const
{
    // ******  Computes Qualcomm CRC-24Q ******
    boost::crc_optimal<24, 0x1864CFBU, 0x0, 0x0, false, false> CRC_RTCM;
    // 1) Converts the string to a vector of uint8_t:
    boost::dynamic_bitset<uint8_t> frame_bits(message_without_crc);
    std::vector<uint8_t> bytes;
    boost::to_block_range(frame_bits, std::back_inserter(bytes));
    std::reverse(bytes.begin(), bytes.end());

    // 2) Computes CRC
    CRC_RTCM.process_bytes(bytes.data(), bytes.size());
    const auto crc_frame = std::bitset<24>(CRC_RTCM.checksum());

    // 3) Builds the complete message
    const std::string complete_message = message_without_crc + crc_frame.to_string();
    return bin_to_binary_data(complete_message);
}


bool Rtcm::check_CRC(const std::string& message) const
{
    boost::crc_optimal<24, 0x1864CFBU, 0x0, 0x0, false, false> CRC_RTCM_CHECK;
    // Convert message to binary
    const std::string message_bin = Rtcm::binary_data_to_bin(message);
    // Check CRC
    const std::string crc = message_bin.substr(message_bin.length() - 24, 24);
    const auto read_crc = std::bitset<24>(crc);
    const std::string msg_without_crc = message_bin.substr(0, message_bin.length() - 24);

    boost::dynamic_bitset<uint8_t> frame_bits(msg_without_crc);
    std::vector<uint8_t> bytes;
    boost::to_block_range(frame_bits, std::back_inserter(bytes));
    std::reverse(bytes.begin(), bytes.end());

    CRC_RTCM_CHECK.process_bytes(bytes.data(), bytes.size());
    const auto computed_crc = std::bitset<24>(CRC_RTCM_CHECK.checksum());
    if (read_crc == computed_crc)
        {
            return true;
        }

    return false;
}


std::string Rtcm::bin_to_binary_data(const std::string& s) const
{
    std::string s_aux;
    const auto remainder = static_cast<int32_t>(std::fmod(s.length(), 8));
    std::vector<uint8_t> c(s.length());

    uint32_t k = 0;
    if (remainder != 0)
        {
            s_aux.assign(s, 0, remainder);
            boost::dynamic_bitset<> rembits(s_aux);
            const uint64_t n = rembits.to_ulong();
            c[0] = static_cast<uint8_t>(n);
            k++;
        }

    const std::size_t start = std::max(remainder, 0);
    for (std::size_t i = start; i < s.length() - 1; i = i + 8)
        {
            s_aux.assign(s, i, 4);
            std::bitset<4> bs(s_aux);
            uint32_t n = bs.to_ulong();
            s_aux.assign(s, i + 4, 4);
            const std::bitset<4> bs2(s_aux);
            const uint32_t n2 = bs2.to_ulong();
            c[k] = static_cast<uint8_t>(n * 16) + static_cast<uint8_t>(n2);
            k++;
        }

    std::string ret(c.begin(), c.begin() + k);
    return ret;
}


std::string Rtcm::binary_data_to_bin(const std::string& s) const
{
    std::string s_aux;
    std::stringstream ss;

    for (char i : s)
        {
            const auto val = static_cast<uint8_t>(i);
            const std::bitset<8> bs(val);
            ss << bs;
        }

    s_aux = ss.str();
    return s_aux;
}


std::string Rtcm::bin_to_hex(const std::string& s) const
{
    std::string s_aux;
    std::stringstream ss;
    const auto remainder = static_cast<int32_t>(std::fmod(s.length(), 4));

    if (remainder != 0)
        {
            s_aux.assign(s, 0, remainder);
            boost::dynamic_bitset<> rembits(s_aux);
            const uint32_t n = rembits.to_ulong();
            ss << std::hex << n;
        }

    const std::size_t start = std::max(remainder, 0);
    for (std::size_t i = start; i < s.length() - 1; i = i + 4)
        {
            s_aux.assign(s, i, 4);
            const std::bitset<4> bs(s_aux);
            const uint32_t n = bs.to_ulong();
            ss << std::hex << n;
        }
    return boost::to_upper_copy(ss.str());
}


std::string Rtcm::hex_to_bin(const std::string& s) const
{
    std::string s_aux;
    s_aux.clear();
    std::stringstream ss;
    ss << s;
    const std::string s_lower = boost::to_upper_copy(ss.str());
    for (size_t i = 0; i < s.length(); i++)
        {
            uint64_t n;
            std::istringstream(s_lower.substr(i, 1)) >> std::hex >> n;
            const std::bitset<4> bs(n);
            s_aux += bs.to_string();
        }
    return s_aux;
}


uint32_t Rtcm::bin_to_uint(const std::string& s) const
{
    if (s.length() > 32)
        {
            LOG(WARNING) << "Cannot convert to a uint32_t";
            return 0;
        }
    const uint32_t reading = strtoul(s.c_str(), nullptr, 2);
    return reading;
}


int32_t Rtcm::bin_to_int(const std::string& s) const
{
    if (s.length() > 32)
        {
            LOG(WARNING) << "Cannot convert to a int32_t";
            return 0;
        }
    int32_t reading;

    // Handle negative numbers
    if (s.substr(0, 1) != "0")
        {
            // Computing two's complement
            boost::dynamic_bitset<> original_bitset(s);
            original_bitset.flip();
            reading = -(original_bitset.to_ulong() + 1);
        }
    else
        {
            reading = strtol(s.c_str(), nullptr, 2);
        }
    return reading;
}


int32_t Rtcm::bin_to_sint(const std::string& s) const
{
    if (s.length() > 32)
        {
            LOG(WARNING) << "Cannot convert to a int32_t";
            return 0;
        }
    int32_t reading;
    int32_t sign;

    // Check for sign bit as defined RTCM doc
    if (s.substr(0, 1) != "0")
        {
            sign = 1;
            // Get the magnitude of the value
            reading = strtol((s.substr(1)).c_str(), nullptr, 2);
        }
    else
        {
            sign = -1;
            // Get the magnitude of the value
            reading = strtol((s.substr(1)).c_str(), nullptr, 2);
        }
    return sign * reading;
}

// Find the sign for glonass data fields (neg = 1, pos = 0)
static inline uint64_t glo_sgn(double val)
{
    if (val < 0)
        {
            return 1;  // If value is negative return 1
        }
    if (val == 0)
        {
            return 0;  // Positive or equal to zero return 0
        }
    return 0;
}


double Rtcm::bin_to_double(const std::string& s) const
{
    double reading;
    if (s.length() > 64)
        {
            LOG(WARNING) << "Cannot convert to a double";
            return 0;
        }

    int64_t reading_int;

    // Handle negative numbers
    if (s.substr(0, 1) != "0")
        {
            // Computing two's complement
            boost::dynamic_bitset<> original_bitset(s);
            original_bitset.flip();
            std::string aux;
            to_string(original_bitset, aux);
            reading_int = -(strtoll(aux.c_str(), nullptr, 2) + 1);
        }
    else
        {
            reading_int = strtoll(s.c_str(), nullptr, 2);
        }

    reading = static_cast<double>(reading_int);
    return reading;
}


uint64_t Rtcm::hex_to_uint(const std::string& s) const
{
    if (s.length() > 32)
        {
            LOG(WARNING) << "Cannot convert to a uint64_t";
            return 0;
        }
    const uint64_t reading = strtoul(s.c_str(), nullptr, 16);
    return reading;
}


int64_t Rtcm::hex_to_int(const std::string& s) const
{
    if (s.length() > 32)
        {
            LOG(WARNING) << "Cannot convert to a int64_t";
            return 0;
        }
    const int64_t reading = strtol(s.c_str(), nullptr, 16);
    return reading;
}


std::string Rtcm::build_message(const std::string& data) const
{
    const uint32_t msg_length_bits = data.length();
    const uint32_t msg_length_bytes = std::ceil(static_cast<float>(msg_length_bits) / 8.0);
    const auto message_length = std::bitset<10>(msg_length_bytes);
    const uint32_t zeros_to_fill = 8 * msg_length_bytes - msg_length_bits;
    const std::string b(zeros_to_fill, '0');
    const std::string msg_content = data + b;
    const std::string msg_without_crc = preamble.to_string() +
                                        reserved_field.to_string() +
                                        message_length.to_string() +
                                        msg_content;
    return Rtcm::add_CRC(msg_without_crc);
}


// *****************************************************************************************************
//
//   MESSAGES AS DEFINED AT RTCM STANDARD 10403.2
//
// *****************************************************************************************************


// ********************************************************
//
//   MESSAGE TYPE 1001 (GPS L1 OBSERVATIONS)
//
// ********************************************************

std::bitset<64> Rtcm::get_MT1001_4_header(uint32_t msg_number, double obs_time, const std::map<int32_t, Gnss_Synchro>& observables,
    uint32_t ref_id, uint32_t smooth_int, bool sync_flag, bool divergence_free)
{
    const uint32_t reference_station_id = ref_id;  // Max: 4095
    const std::map<int32_t, Gnss_Synchro>& observables_ = observables;
    const bool synchronous_GNSS_flag = sync_flag;
    const bool divergence_free_smoothing_indicator = divergence_free;
    const uint32_t smoothing_interval = smooth_int;
    Rtcm::set_DF002(msg_number);
    Rtcm::set_DF003(reference_station_id);
    Rtcm::set_DF004(obs_time);
    Rtcm::set_DF005(synchronous_GNSS_flag);
    Rtcm::set_DF006(observables_);
    Rtcm::set_DF007(divergence_free_smoothing_indicator);
    Rtcm::set_DF008(smoothing_interval);

    const std::string header = DF002.to_string() +
                               DF003.to_string() +
                               DF004.to_string() +
                               DF005.to_string() +
                               DF006.to_string() +
                               DF007.to_string() +
                               DF008.to_string();

    std::bitset<64> header_msg(header);
    return header_msg;
}


std::bitset<58> Rtcm::get_MT1001_sat_content(const Gps_Ephemeris& eph, double obs_time, const Gnss_Synchro& gnss_synchro)
{
    bool code_indicator = false;  // code indicator   0: C/A code   1: P(Y) code direct
    Rtcm::set_DF009(gnss_synchro);
    Rtcm::set_DF010(code_indicator);  // code indicator   0: C/A code   1: P(Y) code direct
    Rtcm::set_DF011(gnss_synchro);
    Rtcm::set_DF012(gnss_synchro);
    Rtcm::set_DF013(eph, obs_time, gnss_synchro);

    const std::string content = DF009.to_string() +
                                DF010.to_string() +
                                DF011.to_string() +
                                DF012.to_string() +
                                DF013.to_string();

    std::bitset<58> content_msg(content);
    return content_msg;
}


std::string Rtcm::print_MT1001(const Gps_Ephemeris& gps_eph, double obs_time, const std::map<int32_t, Gnss_Synchro>& observables, uint16_t station_id)
{
    const auto ref_id = static_cast<uint32_t>(station_id);
    uint32_t smooth_int = 0;
    bool sync_flag = false;
    bool divergence_free = false;

    // Get a map with GPS L1 only observations
    std::map<int32_t, Gnss_Synchro> observablesL1;
    std::map<int32_t, Gnss_Synchro>::const_iterator observables_iter;

    for (observables_iter = observables.cbegin();
        observables_iter != observables.cend();
        observables_iter++)
        {
            const std::string system_(&observables_iter->second.System, 1);
            const std::string sig_(observables_iter->second.Signal);
            if ((system_ == "G") && (sig_ == "1C"))
                {
                    observablesL1.insert(std::pair<int32_t, Gnss_Synchro>(observables_iter->first, observables_iter->second));
                }
        }

    const std::bitset<64> header = Rtcm::get_MT1001_4_header(1001, obs_time, observablesL1, ref_id, smooth_int, sync_flag, divergence_free);
    std::string data = header.to_string();

    for (observables_iter = observablesL1.cbegin();
        observables_iter != observablesL1.cend();
        observables_iter++)
        {
            const std::bitset<58> content = Rtcm::get_MT1001_sat_content(gps_eph, obs_time, observables_iter->second);
            data += content.to_string();
        }

    std::string msg = build_message(data);
    if (server_is_running)
        {
            rtcm_message_queue->push(msg);
        }
    return msg;
}


// ********************************************************
//
//   MESSAGE TYPE 1002 (EXTENDED GPS L1 OBSERVATIONS)
//
// ********************************************************

std::string Rtcm::print_MT1002(const Gps_Ephemeris& gps_eph, double obs_time, const std::map<int32_t, Gnss_Synchro>& observables, uint16_t station_id)
{
    const auto ref_id = static_cast<uint32_t>(station_id);
    uint32_t smooth_int = 0;
    bool sync_flag = false;
    bool divergence_free = false;

    // Get a map with GPS L1 only observations
    std::map<int32_t, Gnss_Synchro> observablesL1;
    std::map<int32_t, Gnss_Synchro>::const_iterator observables_iter;

    for (observables_iter = observables.cbegin();
        observables_iter != observables.cend();
        observables_iter++)
        {
            const std::string system_(&observables_iter->second.System, 1);
            const std::string sig_(observables_iter->second.Signal);
            if ((system_ == "G") && (sig_ == "1C"))
                {
                    observablesL1.insert(std::pair<int32_t, Gnss_Synchro>(observables_iter->first, observables_iter->second));
                }
        }

    const std::bitset<64> header = Rtcm::get_MT1001_4_header(1002, obs_time, observablesL1, ref_id, smooth_int, sync_flag, divergence_free);
    std::string data = header.to_string();

    for (observables_iter = observablesL1.cbegin();
        observables_iter != observablesL1.cend();
        observables_iter++)
        {
            const std::bitset<74> content = Rtcm::get_MT1002_sat_content(gps_eph, obs_time, observables_iter->second);
            data += content.to_string();
        }

    const std::string msg = build_message(data);
    if (server_is_running)
        {
            rtcm_message_queue->push(msg);
        }
    return msg;
}


std::bitset<74> Rtcm::get_MT1002_sat_content(const Gps_Ephemeris& eph, double obs_time, const Gnss_Synchro& gnss_synchro)
{
    bool code_indicator = false;  // code indicator   0: C/A code   1: P(Y) code direct
    Rtcm::set_DF009(gnss_synchro);
    Rtcm::set_DF010(code_indicator);  // code indicator   0: C/A code   1: P(Y) code direct
    Rtcm::set_DF011(gnss_synchro);
    Rtcm::set_DF012(gnss_synchro);
    Rtcm::set_DF013(eph, obs_time, gnss_synchro);

    const std::string content = DF009.to_string() +
                                DF010.to_string() +
                                DF011.to_string() +
                                DF012.to_string() +
                                DF013.to_string() +
                                DF014.to_string() +
                                DF015.to_string();

    std::bitset<74> content_msg(content);
    return content_msg;
}


// ********************************************************
//
//   MESSAGE TYPE 1003 (GPS L1 & L2 OBSERVATIONS)
//
// ********************************************************

std::string Rtcm::print_MT1003(const Gps_Ephemeris& ephL1, const Gps_CNAV_Ephemeris& ephL2, double obs_time, const std::map<int32_t, Gnss_Synchro>& observables, uint16_t station_id)
{
    const auto ref_id = static_cast<uint32_t>(station_id);
    uint32_t smooth_int = 0;
    bool sync_flag = false;
    bool divergence_free = false;

    // Get maps with GPS L1 and L2 observations
    std::map<int32_t, Gnss_Synchro> observablesL1;
    std::map<int32_t, Gnss_Synchro> observablesL2;
    std::map<int32_t, Gnss_Synchro>::const_iterator observables_iter;
    std::map<int32_t, Gnss_Synchro>::const_iterator observables_iter2;

    for (observables_iter = observables.cbegin();
        observables_iter != observables.cend();
        observables_iter++)
        {
            const std::string system_(&observables_iter->second.System, 1);
            const std::string sig_(observables_iter->second.Signal);
            if ((system_ == "G") && (sig_ == "1C"))
                {
                    observablesL1.insert(std::pair<int32_t, Gnss_Synchro>(observables_iter->first, observables_iter->second));
                }
            if ((system_ == "G") && (sig_ == "2S"))
                {
                    observablesL2.insert(std::pair<int32_t, Gnss_Synchro>(observables_iter->first, observables_iter->second));
                }
        }

    // Get common observables
    std::vector<std::pair<Gnss_Synchro, Gnss_Synchro>> common_observables;
    std::vector<std::pair<Gnss_Synchro, Gnss_Synchro>>::const_iterator common_observables_iter;
    std::map<int32_t, Gnss_Synchro> observablesL1_with_L2;

    for (observables_iter = observablesL1.cbegin();
        observables_iter != observablesL1.cend();
        observables_iter++)
        {
            const uint32_t prn_ = observables_iter->second.PRN;
            for (observables_iter2 = observablesL2.cbegin();
                observables_iter2 != observablesL2.cend();
                observables_iter2++)
                {
                    if (observables_iter2->second.PRN == prn_)
                        {
                            std::pair<Gnss_Synchro, Gnss_Synchro> p;
                            Gnss_Synchro pr1 = observables_iter->second;
                            Gnss_Synchro pr2 = observables_iter2->second;
                            p = std::make_pair(pr1, pr2);
                            common_observables.push_back(p);
                            observablesL1_with_L2.insert(std::pair<int32_t, Gnss_Synchro>(observables_iter->first, observables_iter->second));
                        }
                }
        }

    const std::bitset<64> header = Rtcm::get_MT1001_4_header(1003, obs_time, observablesL1_with_L2, ref_id, smooth_int, sync_flag, divergence_free);
    std::string data = header.to_string();

    for (common_observables_iter = common_observables.cbegin();
        common_observables_iter != common_observables.cend();
        common_observables_iter++)
        {
            std::bitset<101> content = Rtcm::get_MT1003_sat_content(ephL1, ephL2, obs_time, common_observables_iter->first, common_observables_iter->second);
            data += content.to_string();
        }

    std::string msg = build_message(data);
    if (server_is_running)
        {
            rtcm_message_queue->push(msg);
        }
    return msg;
}


std::bitset<101> Rtcm::get_MT1003_sat_content(const Gps_Ephemeris& ephL1, const Gps_CNAV_Ephemeris& ephL2, double obs_time, const Gnss_Synchro& gnss_synchroL1, const Gnss_Synchro& gnss_synchroL2)
{
    bool code_indicator = false;  // code indicator   0: C/A code   1: P(Y) code direct
    Rtcm::set_DF009(gnss_synchroL1);
    Rtcm::set_DF010(code_indicator);  // code indicator   0: C/A code   1: P(Y) code direct
    Rtcm::set_DF011(gnss_synchroL1);
    Rtcm::set_DF012(gnss_synchroL1);
    Rtcm::set_DF013(ephL1, obs_time, gnss_synchroL1);
    auto DF016_ = std::bitset<2>(0);  // code indicator   0: C/A or L2C code   1: P(Y) code direct  2:P(Y) code cross-correlated    3: Correlated P/Y
    Rtcm::set_DF017(gnss_synchroL1, gnss_synchroL2);
    Rtcm::set_DF018(gnss_synchroL1, gnss_synchroL2);
    Rtcm::set_DF019(ephL2, obs_time, gnss_synchroL2);

    const std::string content = DF009.to_string() +
                                DF010.to_string() +
                                DF011.to_string() +
                                DF012.to_string() +
                                DF013.to_string() +
                                DF016_.to_string() +
                                DF017.to_string() +
                                DF018.to_string() +
                                DF019.to_string();

    std::bitset<101> content_msg(content);
    return content_msg;
}


// ******************************************************************
//
//   MESSAGE TYPE 1004 (EXTENDED GPS L1 & L2 OBSERVATIONS)
//
// ******************************************************************

std::string Rtcm::print_MT1004(const Gps_Ephemeris& ephL1, const Gps_CNAV_Ephemeris& ephL2, double obs_time, const std::map<int32_t, Gnss_Synchro>& observables, uint16_t station_id)
{
    const auto ref_id = static_cast<uint32_t>(station_id);
    uint32_t smooth_int = 0;
    bool sync_flag = false;
    bool divergence_free = false;

    // Get maps with GPS L1 and L2 observations
    std::map<int32_t, Gnss_Synchro> observablesL1;
    std::map<int32_t, Gnss_Synchro> observablesL2;
    std::map<int32_t, Gnss_Synchro>::const_iterator observables_iter;
    std::map<int32_t, Gnss_Synchro>::const_iterator observables_iter2;

    for (observables_iter = observables.cbegin();
        observables_iter != observables.cend();
        observables_iter++)
        {
            const std::string system_(&observables_iter->second.System, 1);
            const std::string sig_(observables_iter->second.Signal);
            if ((system_ == "G") && (sig_ == "1C"))
                {
                    observablesL1.insert(std::pair<int32_t, Gnss_Synchro>(observables_iter->first, observables_iter->second));
                }
            if ((system_ == "G") && (sig_ == "2S"))
                {
                    observablesL2.insert(std::pair<int32_t, Gnss_Synchro>(observables_iter->first, observables_iter->second));
                }
        }

    // Get common observables
    std::vector<std::pair<Gnss_Synchro, Gnss_Synchro>> common_observables;
    std::vector<std::pair<Gnss_Synchro, Gnss_Synchro>>::const_iterator common_observables_iter;
    std::map<int32_t, Gnss_Synchro> observablesL1_with_L2;

    for (observables_iter = observablesL1.cbegin();
        observables_iter != observablesL1.cend();
        observables_iter++)
        {
            const uint32_t prn_ = observables_iter->second.PRN;
            for (observables_iter2 = observablesL2.cbegin();
                observables_iter2 != observablesL2.cend();
                observables_iter2++)
                {
                    if (observables_iter2->second.PRN == prn_)
                        {
                            std::pair<Gnss_Synchro, Gnss_Synchro> p;
                            Gnss_Synchro pr1 = observables_iter->second;
                            Gnss_Synchro pr2 = observables_iter2->second;
                            p = std::make_pair(pr1, pr2);
                            common_observables.push_back(p);
                            observablesL1_with_L2.insert(std::pair<int32_t, Gnss_Synchro>(observables_iter->first, observables_iter->second));
                        }
                }
        }

    const std::bitset<64> header = Rtcm::get_MT1001_4_header(1004, obs_time, observablesL1_with_L2, ref_id, smooth_int, sync_flag, divergence_free);
    std::string data = header.to_string();

    for (common_observables_iter = common_observables.cbegin();
        common_observables_iter != common_observables.cend();
        common_observables_iter++)
        {
            std::bitset<125> content = Rtcm::get_MT1004_sat_content(ephL1, ephL2, obs_time, common_observables_iter->first, common_observables_iter->second);
            data += content.to_string();
        }

    std::string msg = build_message(data);
    if (server_is_running)
        {
            rtcm_message_queue->push(msg);
        }
    return msg;
}


std::bitset<125> Rtcm::get_MT1004_sat_content(const Gps_Ephemeris& ephL1, const Gps_CNAV_Ephemeris& ephL2, double obs_time, const Gnss_Synchro& gnss_synchroL1, const Gnss_Synchro& gnss_synchroL2)
{
    bool code_indicator = false;  // code indicator   0: C/A code   1: P(Y) code direct
    Rtcm::set_DF009(gnss_synchroL1);
    Rtcm::set_DF010(code_indicator);  // code indicator   0: C/A code   1: P(Y) code direct
    Rtcm::set_DF011(gnss_synchroL1);
    Rtcm::set_DF012(gnss_synchroL1);
    Rtcm::set_DF013(ephL1, obs_time, gnss_synchroL1);
    Rtcm::set_DF014(gnss_synchroL1);
    Rtcm::set_DF015(gnss_synchroL1);
    auto DF016_ = std::bitset<2>(0);  // code indicator   0: C/A or L2C code   1: P(Y) code direct  2:P(Y) code cross-correlated    3: Correlated P/Y
    Rtcm::set_DF017(gnss_synchroL1, gnss_synchroL2);
    Rtcm::set_DF018(gnss_synchroL1, gnss_synchroL2);
    Rtcm::set_DF019(ephL2, obs_time, gnss_synchroL2);
    Rtcm::set_DF020(gnss_synchroL2);

    const std::string content = DF009.to_string() +
                                DF010.to_string() +
                                DF011.to_string() +
                                DF012.to_string() +
                                DF013.to_string() +
                                DF014.to_string() +
                                DF015.to_string() +
                                DF016_.to_string() +
                                DF017.to_string() +
                                DF018.to_string() +
                                DF019.to_string() +
                                DF020.to_string();

    std::bitset<125> content_msg(content);
    return content_msg;
}


// ********************************************************
//
//   MESSAGE TYPE 1005 (STATION DESCRIPTION)
//
// ********************************************************


/* Stationary Antenna Reference Point, No Height Information
 * Reference Station Id = 2003
   GPS Service supported, but not GLONASS or Galileo
   ARP ECEF-X = 1114104.5999 meters
   ARP ECEF-Y = -4850729.7108 meters
   ARP ECEF-Z = 3975521.4643 meters
   Expected output: D3 00 13 3E D7 D3 02 02 98 0E DE EF 34 B4 BD 62
                    AC 09 41 98 6F 33 36 0B 98
 */
std::bitset<152> Rtcm::get_MT1005_test()
{
    const uint32_t mt1005 = 1005;
    const uint32_t reference_station_id = 2003;  // Max: 4095
    const double ECEF_X = 1114104.5999;          // units: m
    const double ECEF_Y = -4850729.7108;         // units: m
    const double ECEF_Z = 3975521.4643;          // units: m

    std::bitset<1> DF001_;

    Rtcm::set_DF002(mt1005);
    Rtcm::set_DF003(reference_station_id);
    Rtcm::set_DF021();
    Rtcm::set_DF022(true);         // GPS
    Rtcm::set_DF023(false);        // Glonass
    Rtcm::set_DF024(false);        // Galileo
    DF141 = std::bitset<1>("0");   // 0: Real, physical reference station
    DF001_ = std::bitset<1>("0");  // Reserved, set to 0
    Rtcm::set_DF025(ECEF_X);
    DF142 = std::bitset<1>("0");  // Single Receiver Oscillator Indicator
    Rtcm::set_DF026(ECEF_Y);
    DF364 = std::bitset<2>("00");  // Quarter Cycle Indicator
    Rtcm::set_DF027(ECEF_Z);

    const std::string message = DF002.to_string() +
                                DF003.to_string() +
                                DF021.to_string() +
                                DF022.to_string() +
                                DF023.to_string() +
                                DF024.to_string() +
                                DF141.to_string() +
                                DF025.to_string() +
                                DF142.to_string() +
                                DF001_.to_string() +
                                DF026.to_string() +
                                DF364.to_string() +
                                DF027.to_string();

    std::bitset<152> test_msg(message);
    return test_msg;
}


std::string Rtcm::print_MT1005(uint32_t ref_id, double ecef_x, double ecef_y, double ecef_z, bool gps, bool glonass, bool galileo, bool non_physical, bool single_oscillator, uint32_t quarter_cycle_indicator)
{
    const uint32_t msg_number = 1005;
    std::bitset<1> DF001_;

    Rtcm::set_DF002(msg_number);
    Rtcm::set_DF003(ref_id);
    Rtcm::set_DF021();
    Rtcm::set_DF022(gps);
    Rtcm::set_DF023(glonass);
    Rtcm::set_DF024(galileo);
    DF141 = std::bitset<1>(non_physical);
    DF001_ = std::bitset<1>("0");
    Rtcm::set_DF025(ecef_x);
    DF142 = std::bitset<1>(single_oscillator);
    Rtcm::set_DF026(ecef_y);
    DF364 = std::bitset<2>(quarter_cycle_indicator);
    Rtcm::set_DF027(ecef_z);

    const std::string data = DF002.to_string() +
                             DF003.to_string() +
                             DF021.to_string() +
                             DF022.to_string() +
                             DF023.to_string() +
                             DF024.to_string() +
                             DF141.to_string() +
                             DF025.to_string() +
                             DF142.to_string() +
                             DF001_.to_string() +
                             DF026.to_string() +
                             DF364.to_string() +
                             DF027.to_string();

    std::string msg = build_message(data);
    if (server_is_running)
        {
            rtcm_message_queue->push(msg);
        }
    return msg;
}


int32_t Rtcm::read_MT1005(const std::string& message, uint32_t& ref_id, double& ecef_x, double& ecef_y, double& ecef_z, bool& gps, bool& glonass, bool& galileo)
{
    // Convert message to binary
    const std::string message_bin = Rtcm::binary_data_to_bin(message);

    if (!Rtcm::check_CRC(message))
        {
            LOG(WARNING) << " Bad CRC detected in RTCM message MT1005";
            return 1;
        }

    // Check than the message number is correct
    const uint32_t preamble_length = 8;
    const uint32_t reserved_field_length = 6;
    uint32_t index = preamble_length + reserved_field_length;

    uint32_t read_message_length = Rtcm::bin_to_uint(message_bin.substr(index, 10));
    index += 10;
    if (read_message_length != 19)
        {
            LOG(WARNING) << " Message MT1005 with wrong length (19 bytes expected, " << read_message_length << " received)";
            return 1;
        }

    const uint32_t msg_number = 1005;
    Rtcm::set_DF002(msg_number);
    std::bitset<12> read_msg_number(message_bin.substr(index, 12));
    index += 12;

    if (DF002 != read_msg_number)
        {
            LOG(WARNING) << " This is not a MT1005 message";
            return 1;
        }

    ref_id = Rtcm::bin_to_uint(message_bin.substr(index, 12));
    index += 12;

    index += 6;  // ITRF year
    gps = static_cast<bool>(Rtcm::bin_to_uint(message_bin.substr(index, 1)));
    index += 1;

    glonass = static_cast<bool>(Rtcm::bin_to_uint(message_bin.substr(index, 1)));
    index += 1;

    galileo = static_cast<bool>(Rtcm::bin_to_uint(message_bin.substr(index, 1)));
    index += 1;

    index += 1;  // ref_station_indicator

    ecef_x = Rtcm::bin_to_double(message_bin.substr(index, 38)) / 10000.0;
    index += 38;

    index += 1;  // single rx oscillator
    index += 1;  // reserved

    ecef_y = Rtcm::bin_to_double(message_bin.substr(index, 38)) / 10000.0;
    index += 38;

    index += 2;  // quarter cycle indicator
    ecef_z = Rtcm::bin_to_double(message_bin.substr(index, 38)) / 10000.0;

    return 0;
}


std::string Rtcm::print_MT1005_test()
{
    std::bitset<152> mt1005 = get_MT1005_test();
    return Rtcm::build_message(mt1005.to_string());
}

// ********************************************************
//
//   MESSAGE TYPE 1006 (STATION DESCRIPTION PLUS HEIGHT INFORMATION)
//
// ********************************************************

std::string Rtcm::print_MT1006(uint32_t ref_id, double ecef_x, double ecef_y, double ecef_z, bool gps, bool glonass, bool galileo, bool non_physical, bool single_oscillator, uint32_t quarter_cycle_indicator, double height)
{
    const uint32_t msg_number = 1006;
    std::bitset<1> DF001_;

    Rtcm::set_DF002(msg_number);
    Rtcm::set_DF003(ref_id);
    Rtcm::set_DF021();
    Rtcm::set_DF022(gps);
    Rtcm::set_DF023(glonass);
    Rtcm::set_DF024(galileo);
    DF141 = std::bitset<1>(non_physical);
    DF001_ = std::bitset<1>("0");
    Rtcm::set_DF025(ecef_x);
    DF142 = std::bitset<1>(single_oscillator);
    Rtcm::set_DF026(ecef_y);
    DF364 = std::bitset<2>(quarter_cycle_indicator);
    Rtcm::set_DF027(ecef_z);
    Rtcm::set_DF028(height);

    const std::string data = DF002.to_string() +
                             DF003.to_string() +
                             DF021.to_string() +
                             DF022.to_string() +
                             DF023.to_string() +
                             DF024.to_string() +
                             DF141.to_string() +
                             DF025.to_string() +
                             DF142.to_string() +
                             DF001_.to_string() +
                             DF026.to_string() +
                             DF364.to_string() +
                             DF027.to_string() +
                             DF028.to_string();

    std::string msg = build_message(data);
    if (server_is_running)
        {
            rtcm_message_queue->push(msg);
        }
    return msg;
}


// ********************************************************
//
//   MESSAGE TYPE 1008 (ANTENNA DESCRIPTOR & SERIAL NUMBER)
//
// ********************************************************
std::string Rtcm::print_MT1008(uint32_t ref_id, const std::string& antenna_descriptor, uint32_t antenna_setup_id, const std::string& antenna_serial_number)
{
    const uint32_t msg_number = 1008;
    auto DF002_ = std::bitset<12>(msg_number);
    Rtcm::set_DF003(ref_id);
    std::string ant_descriptor = antenna_descriptor;
    uint32_t len = ant_descriptor.length();
    if (len > 31)
        {
            ant_descriptor = ant_descriptor.substr(0, 31);
            len = 31;
        }
    DF029 = std::bitset<8>(len);

    std::string DF030_str_;
    for (char c : ant_descriptor)
        {
            const auto character = std::bitset<8>(c);
            DF030_str_ += character.to_string();
        }

    Rtcm::set_DF031(antenna_setup_id);

    std::string ant_sn(antenna_serial_number);
    uint32_t len2 = ant_sn.length();
    if (len2 > 31)
        {
            ant_sn = ant_sn.substr(0, 31);
            len2 = 31;
        }
    DF032 = std::bitset<8>(len2);

    std::string DF033_str_;
    for (char c : ant_sn)
        {
            const auto character = std::bitset<8>(c);
            DF033_str_ += character.to_string();
        }

    const std::string data = DF002_.to_string() +
                             DF003.to_string() +
                             DF029.to_string() +
                             DF030_str_ +
                             DF031.to_string() +
                             DF032.to_string() +
                             DF033_str_;

    std::string msg = build_message(data);
    if (server_is_running)
        {
            rtcm_message_queue->push(msg);
        }
    return msg;
}


// ********************************************************
//
//   MESSAGE TYPE 1009 (GLONASS L1 Basic RTK Observables)
//
// ********************************************************
std::bitset<61> Rtcm::get_MT1009_12_header(uint32_t msg_number, double obs_time, const std::map<int32_t, Gnss_Synchro>& observables,
    uint32_t ref_id, uint32_t smooth_int, bool sync_flag, bool divergence_free)
{
    const uint32_t reference_station_id = ref_id;  // Max: 4095
    const std::map<int32_t, Gnss_Synchro>& observables_ = observables;
    const bool synchronous_GNSS_flag = sync_flag;
    const bool divergence_free_smoothing_indicator = divergence_free;
    const uint32_t smoothing_interval = smooth_int;
    Rtcm::set_DF002(msg_number);
    Rtcm::set_DF003(reference_station_id);
    Rtcm::set_DF034(obs_time);
    Rtcm::set_DF005(synchronous_GNSS_flag);
    Rtcm::set_DF035(observables_);
    Rtcm::set_DF036(divergence_free_smoothing_indicator);
    Rtcm::set_DF037(smoothing_interval);

    const std::string header = DF002.to_string() +
                               DF003.to_string() +
                               DF034.to_string() +
                               DF005.to_string() +
                               DF035.to_string() +
                               DF036.to_string() +
                               DF037.to_string();

    std::bitset<61> header_msg(header);
    return header_msg;
}


std::bitset<64> Rtcm::get_MT1009_sat_content(const Glonass_Gnav_Ephemeris& eph, double obs_time, const Gnss_Synchro& gnss_synchro)
{
    const bool code_indicator = false;  // code indicator   0: C/A code   1: P(Y) code direct
    Rtcm::set_DF038(gnss_synchro);
    Rtcm::set_DF039(code_indicator);
    Rtcm::set_DF040(eph.i_satellite_freq_channel);
    Rtcm::set_DF041(gnss_synchro);
    Rtcm::set_DF042(gnss_synchro);
    Rtcm::set_DF043(eph, obs_time, gnss_synchro);

    const std::string content = DF038.to_string() +
                                DF039.to_string() +
                                DF040.to_string() +
                                DF041.to_string() +
                                DF042.to_string() +
                                DF043.to_string();

    std::bitset<64> content_msg(content);
    return content_msg;
}


std::string Rtcm::print_MT1009(const Glonass_Gnav_Ephemeris& glonass_gnav_eph, double obs_time, const std::map<int32_t, Gnss_Synchro>& observables, uint16_t station_id)
{
    const auto ref_id = static_cast<uint32_t>(station_id);
    uint32_t smooth_int = 0;
    bool sync_flag = false;
    bool divergence_free = false;

    // Get a map with GLONASS L1 only observations
    std::map<int32_t, Gnss_Synchro> observablesL1;
    std::map<int32_t, Gnss_Synchro>::const_iterator observables_iter;

    for (observables_iter = observables.begin();
        observables_iter != observables.end();
        observables_iter++)
        {
            const std::string system_(&observables_iter->second.System, 1);
            const std::string sig_(observables_iter->second.Signal);
            if ((system_ == "R") && (sig_ == "1C"))
                {
                    observablesL1.insert(std::pair<int32_t, Gnss_Synchro>(observables_iter->first, observables_iter->second));
                }
        }

    const std::bitset<61> header = Rtcm::get_MT1009_12_header(1009, obs_time, observablesL1, ref_id, smooth_int, sync_flag, divergence_free);
    std::string data = header.to_string();

    for (observables_iter = observablesL1.begin();
        observables_iter != observablesL1.end();
        observables_iter++)
        {
            const std::bitset<64> content = Rtcm::get_MT1009_sat_content(glonass_gnav_eph, obs_time, observables_iter->second);
            data += content.to_string();
        }

    std::string msg = build_message(data);
    if (server_is_running)
        {
            rtcm_message_queue->push(msg);
        }
    return msg;
}


// ********************************************************
//
//   MESSAGE TYPE 1010 (EXTENDED GLONASS L1 OBSERVATIONS)
//
// ********************************************************

std::string Rtcm::print_MT1010(const Glonass_Gnav_Ephemeris& glonass_gnav_eph, double obs_time, const std::map<int32_t, Gnss_Synchro>& observables, uint16_t station_id)
{
    const auto ref_id = static_cast<uint32_t>(station_id);
    uint32_t smooth_int = 0;
    bool sync_flag = false;
    bool divergence_free = false;

    // Get a map with GPS L1 only observations
    std::map<int32_t, Gnss_Synchro> observablesL1;
    std::map<int32_t, Gnss_Synchro>::const_iterator observables_iter;

    for (observables_iter = observables.begin();
        observables_iter != observables.end();
        observables_iter++)
        {
            const std::string system_(&observables_iter->second.System, 1);
            const std::string sig_(observables_iter->second.Signal);
            if ((system_ == "R") && (sig_ == "1C"))
                {
                    observablesL1.insert(std::pair<int32_t, Gnss_Synchro>(observables_iter->first, observables_iter->second));
                }
        }

    const std::bitset<61> header = Rtcm::get_MT1009_12_header(1010, obs_time, observablesL1, ref_id, smooth_int, sync_flag, divergence_free);
    std::string data = header.to_string();

    for (observables_iter = observablesL1.begin();
        observables_iter != observablesL1.end();
        observables_iter++)
        {
            const std::bitset<79> content = Rtcm::get_MT1010_sat_content(glonass_gnav_eph, obs_time, observables_iter->second);
            data += content.to_string();
        }

    std::string msg = build_message(data);
    if (server_is_running)
        {
            rtcm_message_queue->push(msg);
        }
    return msg;
}


std::bitset<79> Rtcm::get_MT1010_sat_content(const Glonass_Gnav_Ephemeris& eph, double obs_time, const Gnss_Synchro& gnss_synchro)
{
    const bool code_indicator = false;  // code indicator   0: C/A code   1: P(Y) code direct
    Rtcm::set_DF038(gnss_synchro);
    Rtcm::set_DF039(code_indicator);
    Rtcm::set_DF040(eph.i_satellite_freq_channel);
    Rtcm::set_DF041(gnss_synchro);
    Rtcm::set_DF042(gnss_synchro);
    Rtcm::set_DF043(eph, obs_time, gnss_synchro);
    Rtcm::set_DF044(gnss_synchro);
    Rtcm::set_DF045(gnss_synchro);

    const std::string content = DF038.to_string() +
                                DF039.to_string() +
                                DF040.to_string() +
                                DF041.to_string() +
                                DF042.to_string() +
                                DF043.to_string() +
                                DF044.to_string() +
                                DF045.to_string();

    std::bitset<79> content_msg(content);
    return content_msg;
}


// ********************************************************
//
//   MESSAGE TYPE 1011 (GLONASS L1 & L2 OBSERVATIONS)
//
// ********************************************************

std::string Rtcm::print_MT1011(const Glonass_Gnav_Ephemeris& ephL1, const Glonass_Gnav_Ephemeris& ephL2, double obs_time, const std::map<int32_t, Gnss_Synchro>& observables, uint16_t station_id)
{
    const auto ref_id = static_cast<uint32_t>(station_id);
    uint32_t smooth_int = 0;
    bool sync_flag = false;
    bool divergence_free = false;

    // Get maps with GPS L1 and L2 observations
    std::map<int32_t, Gnss_Synchro> observablesL1;
    std::map<int32_t, Gnss_Synchro> observablesL2;
    std::map<int32_t, Gnss_Synchro>::const_iterator observables_iter;
    std::map<int32_t, Gnss_Synchro>::const_iterator observables_iter2;

    for (observables_iter = observables.begin();
        observables_iter != observables.end();
        observables_iter++)
        {
            const std::string system_(&observables_iter->second.System, 1);
            const std::string sig_(observables_iter->second.Signal);
            if ((system_ == "R") && (sig_ == "1C"))
                {
                    observablesL1.insert(std::pair<int32_t, Gnss_Synchro>(observables_iter->first, observables_iter->second));
                }
            if ((system_ == "R") && (sig_ == "2C"))
                {
                    observablesL2.insert(std::pair<int32_t, Gnss_Synchro>(observables_iter->first, observables_iter->second));
                }
        }

    // Get common observables
    std::vector<std::pair<Gnss_Synchro, Gnss_Synchro>> common_observables;
    std::vector<std::pair<Gnss_Synchro, Gnss_Synchro>>::const_iterator common_observables_iter;
    std::map<int32_t, Gnss_Synchro> observablesL1_with_L2;

    for (observables_iter = observablesL1.begin();
        observables_iter != observablesL1.end();
        observables_iter++)
        {
            const uint32_t prn_ = observables_iter->second.PRN;
            for (observables_iter2 = observablesL2.begin();
                observables_iter2 != observablesL2.end();
                observables_iter2++)
                {
                    if (observables_iter2->second.PRN == prn_)
                        {
                            std::pair<Gnss_Synchro, Gnss_Synchro> p;
                            Gnss_Synchro pr1 = observables_iter->second;
                            Gnss_Synchro pr2 = observables_iter2->second;
                            p = std::make_pair(pr1, pr2);
                            common_observables.push_back(p);
                            observablesL1_with_L2.insert(std::pair<int32_t, Gnss_Synchro>(observables_iter->first, observables_iter->second));
                        }
                }
        }

    const std::bitset<61> header = Rtcm::get_MT1009_12_header(1011, obs_time, observablesL1_with_L2, ref_id, smooth_int, sync_flag, divergence_free);
    std::string data = header.to_string();

    for (common_observables_iter = common_observables.begin();
        common_observables_iter != common_observables.end();
        common_observables_iter++)
        {
            const std::bitset<107> content = Rtcm::get_MT1011_sat_content(ephL1, ephL2, obs_time, common_observables_iter->first, common_observables_iter->second);
            data += content.to_string();
        }

    std::string msg = build_message(data);
    if (server_is_running)
        {
            rtcm_message_queue->push(msg);
        }
    return msg;
}


std::bitset<107> Rtcm::get_MT1011_sat_content(const Glonass_Gnav_Ephemeris& ephL1, const Glonass_Gnav_Ephemeris& ephL2, double obs_time, const Gnss_Synchro& gnss_synchroL1, const Gnss_Synchro& gnss_synchroL2)
{
    const bool code_indicator = false;  // code indicator   0: C/A code   1: P(Y) code direct
    Rtcm::set_DF038(gnss_synchroL1);
    Rtcm::set_DF039(code_indicator);
    Rtcm::set_DF040(ephL1.i_satellite_freq_channel);
    Rtcm::set_DF041(gnss_synchroL1);
    Rtcm::set_DF042(gnss_synchroL1);
    Rtcm::set_DF043(ephL1, obs_time, gnss_synchroL1);
    auto DF046_ = std::bitset<2>(0);  // code indicator   0: C/A or L2C code   1: P(Y) code direct  2:P(Y) code cross-correlated    3: Correlated P/Y
    Rtcm::set_DF047(gnss_synchroL1, gnss_synchroL2);
    Rtcm::set_DF048(gnss_synchroL1, gnss_synchroL2);
    Rtcm::set_DF049(ephL2, obs_time, gnss_synchroL2);

    const std::string content = DF038.to_string() +
                                DF039.to_string() +
                                DF040.to_string() +
                                DF041.to_string() +
                                DF042.to_string() +
                                DF043.to_string() +
                                DF046_.to_string() +
                                DF047.to_string() +
                                DF048.to_string() +
                                DF049.to_string();

    std::bitset<107> content_msg(content);
    return content_msg;
}


// ******************************************************************
//
//   MESSAGE TYPE 1004 (EXTENDED GLONASS L1 & L2 OBSERVATIONS)
//
// ******************************************************************

std::string Rtcm::print_MT1012(const Glonass_Gnav_Ephemeris& ephL1, const Glonass_Gnav_Ephemeris& ephL2, double obs_time, const std::map<int32_t, Gnss_Synchro>& observables, uint16_t station_id)
{
    const auto ref_id = static_cast<uint32_t>(station_id);
    uint32_t smooth_int = 0;
    bool sync_flag = false;
    bool divergence_free = false;

    // Get maps with GLONASS L1 and L2 observations
    std::map<int32_t, Gnss_Synchro> observablesL1;
    std::map<int32_t, Gnss_Synchro> observablesL2;
    std::map<int32_t, Gnss_Synchro>::const_iterator observables_iter;
    std::map<int32_t, Gnss_Synchro>::const_iterator observables_iter2;

    for (observables_iter = observables.begin();
        observables_iter != observables.end();
        observables_iter++)
        {
            const std::string system_(&observables_iter->second.System, 1);
            const std::string sig_(observables_iter->second.Signal);
            if ((system_ == "R") && (sig_ == "1C"))
                {
                    observablesL1.insert(std::pair<int32_t, Gnss_Synchro>(observables_iter->first, observables_iter->second));
                }
            if ((system_ == "R") && (sig_ == "2C"))
                {
                    observablesL2.insert(std::pair<int32_t, Gnss_Synchro>(observables_iter->first, observables_iter->second));
                }
        }

    // Get common observables
    std::vector<std::pair<Gnss_Synchro, Gnss_Synchro>> common_observables;
    std::vector<std::pair<Gnss_Synchro, Gnss_Synchro>>::const_iterator common_observables_iter;
    std::map<int32_t, Gnss_Synchro> observablesL1_with_L2;

    for (observables_iter = observablesL1.begin();
        observables_iter != observablesL1.end();
        observables_iter++)
        {
            const uint32_t prn_ = observables_iter->second.PRN;
            for (observables_iter2 = observablesL2.begin();
                observables_iter2 != observablesL2.end();
                observables_iter2++)
                {
                    if (observables_iter2->second.PRN == prn_)
                        {
                            std::pair<Gnss_Synchro, Gnss_Synchro> p;
                            Gnss_Synchro pr1 = observables_iter->second;
                            Gnss_Synchro pr2 = observables_iter2->second;
                            p = std::make_pair(pr1, pr2);
                            common_observables.push_back(p);
                            observablesL1_with_L2.insert(std::pair<int32_t, Gnss_Synchro>(observables_iter->first, observables_iter->second));
                        }
                }
        }

    const std::bitset<61> header = Rtcm::get_MT1009_12_header(1012, obs_time, observablesL1_with_L2, ref_id, smooth_int, sync_flag, divergence_free);
    std::string data = header.to_string();

    for (common_observables_iter = common_observables.begin();
        common_observables_iter != common_observables.end();
        common_observables_iter++)
        {
            const std::bitset<130> content = Rtcm::get_MT1012_sat_content(ephL1, ephL2, obs_time, common_observables_iter->first, common_observables_iter->second);
            data += content.to_string();
        }

    std::string msg = build_message(data);
    if (server_is_running)
        {
            rtcm_message_queue->push(msg);
        }
    return msg;
}


std::bitset<130> Rtcm::get_MT1012_sat_content(const Glonass_Gnav_Ephemeris& ephL1, const Glonass_Gnav_Ephemeris& ephL2, double obs_time, const Gnss_Synchro& gnss_synchroL1, const Gnss_Synchro& gnss_synchroL2)
{
    const bool code_indicator = false;  // code indicator   0: C/A code   1: P(Y) code direct
    Rtcm::set_DF038(gnss_synchroL1);
    Rtcm::set_DF039(code_indicator);
    Rtcm::set_DF040(ephL1.i_satellite_freq_channel);
    Rtcm::set_DF041(gnss_synchroL1);
    Rtcm::set_DF042(gnss_synchroL1);
    Rtcm::set_DF043(ephL1, obs_time, gnss_synchroL1);
    Rtcm::set_DF044(gnss_synchroL1);
    Rtcm::set_DF045(gnss_synchroL1);
    auto DF046_ = std::bitset<2>(0);  // code indicator   0: C/A or L2C code   1: P(Y) code direct  2:P(Y) code cross-correlated    3: Correlated P/Y
    Rtcm::set_DF047(gnss_synchroL1, gnss_synchroL2);
    Rtcm::set_DF048(gnss_synchroL1, gnss_synchroL2);
    Rtcm::set_DF049(ephL2, obs_time, gnss_synchroL2);
    Rtcm::set_DF050(gnss_synchroL2);

    const std::string content = DF038.to_string() +
                                DF039.to_string() +
                                DF040.to_string() +
                                DF041.to_string() +
                                DF042.to_string() +
                                DF043.to_string() +
                                DF044.to_string() +
                                DF045.to_string() +
                                DF046_.to_string() +
                                DF047.to_string() +
                                DF048.to_string() +
                                DF049.to_string() +
                                DF050.to_string();

    std::bitset<130> content_msg(content);
    return content_msg;
}


// ********************************************************
//
//   MESSAGE TYPE 1019 (GPS EPHEMERIS)
//
// ********************************************************

std::string Rtcm::print_MT1019(const Gps_Ephemeris& gps_eph)
{
    const uint32_t msg_number = 1019;

    Rtcm::set_DF002(msg_number);
    Rtcm::set_DF009(gps_eph);
    Rtcm::set_DF076(gps_eph);
    Rtcm::set_DF077(gps_eph);
    Rtcm::set_DF078(gps_eph);
    Rtcm::set_DF079(gps_eph);
    Rtcm::set_DF071(gps_eph);
    Rtcm::set_DF081(gps_eph);
    Rtcm::set_DF082(gps_eph);
    Rtcm::set_DF083(gps_eph);
    Rtcm::set_DF084(gps_eph);
    Rtcm::set_DF085(gps_eph);
    Rtcm::set_DF086(gps_eph);
    Rtcm::set_DF087(gps_eph);
    Rtcm::set_DF088(gps_eph);
    Rtcm::set_DF089(gps_eph);
    Rtcm::set_DF090(gps_eph);
    Rtcm::set_DF091(gps_eph);
    Rtcm::set_DF092(gps_eph);
    Rtcm::set_DF093(gps_eph);
    Rtcm::set_DF094(gps_eph);
    Rtcm::set_DF095(gps_eph);
    Rtcm::set_DF096(gps_eph);
    Rtcm::set_DF097(gps_eph);
    Rtcm::set_DF098(gps_eph);
    Rtcm::set_DF099(gps_eph);
    Rtcm::set_DF100(gps_eph);
    Rtcm::set_DF101(gps_eph);
    Rtcm::set_DF102(gps_eph);
    Rtcm::set_DF103(gps_eph);
    Rtcm::set_DF137(gps_eph);

    const std::string data = DF002.to_string() +
                             DF009.to_string() +
                             DF076.to_string() +
                             DF077.to_string() +
                             DF078.to_string() +
                             DF079.to_string() +
                             DF071.to_string() +
                             DF081.to_string() +
                             DF082.to_string() +
                             DF083.to_string() +
                             DF084.to_string() +
                             DF085.to_string() +
                             DF086.to_string() +
                             DF087.to_string() +
                             DF088.to_string() +
                             DF089.to_string() +
                             DF090.to_string() +
                             DF091.to_string() +
                             DF092.to_string() +
                             DF093.to_string() +
                             DF094.to_string() +
                             DF095.to_string() +
                             DF096.to_string() +
                             DF097.to_string() +
                             DF098.to_string() +
                             DF099.to_string() +
                             DF100.to_string() +
                             DF101.to_string() +
                             DF102.to_string() +
                             DF103.to_string() +
                             DF137.to_string();

    if (data.length() != 488)
        {
            LOG(WARNING) << "Bad-formatted RTCM MT1019 (488 bits expected, found " << data.length() << ")";
        }

    std::string msg = build_message(data);
    if (server_is_running)
        {
            rtcm_message_queue->push(msg);
        }
    return msg;
}


int32_t Rtcm::read_MT1019(const std::string& message, Gps_Ephemeris& gps_eph) const
{
    // Convert message to binary
    const std::string message_bin = Rtcm::binary_data_to_bin(message);

    if (!Rtcm::check_CRC(message))
        {
            LOG(WARNING) << " Bad CRC detected in RTCM message MT1019";
            return 1;
        }

    const uint32_t preamble_length = 8;
    const uint32_t reserved_field_length = 6;
    uint32_t index = preamble_length + reserved_field_length;

    const uint32_t read_message_length = Rtcm::bin_to_uint(message_bin.substr(index, 10));
    index += 10;

    if (read_message_length != 61)
        {
            LOG(WARNING) << " Message MT1019 seems too long (61 bytes expected, " << read_message_length << " received)";
            return 1;
        }

    // Check than the message number is correct
    const uint32_t read_msg_number = Rtcm::bin_to_uint(message_bin.substr(index, 12));
    index += 12;

    if (1019 != read_msg_number)
        {
            LOG(WARNING) << " This is not a MT1019 message";
            return 1;
        }

    // Fill Gps Ephemeris with message data content
    gps_eph.PRN = Rtcm::bin_to_uint(message_bin.substr(index, 6));
    index += 6;

    gps_eph.WN = static_cast<int32_t>(Rtcm::bin_to_uint(message_bin.substr(index, 10)));
    index += 10;

    gps_eph.SV_accuracy = static_cast<int32_t>(Rtcm::bin_to_uint(message_bin.substr(index, 4)));
    index += 4;

    gps_eph.code_on_L2 = static_cast<int32_t>(Rtcm::bin_to_uint(message_bin.substr(index, 2)));
    index += 2;

    gps_eph.idot = static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 14))) * I_DOT_LSB;
    index += 14;

    gps_eph.IODE_SF2 = static_cast<double>(Rtcm::bin_to_uint(message_bin.substr(index, 8)));
    gps_eph.IODE_SF3 = static_cast<double>(Rtcm::bin_to_uint(message_bin.substr(index, 8)));
    index += 8;

    gps_eph.toc = static_cast<double>(Rtcm::bin_to_uint(message_bin.substr(index, 16))) * T_OC_LSB;
    index += 16;

    gps_eph.af2 = static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 8))) * A_F2_LSB;
    index += 8;

    gps_eph.af1 = static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 16))) * A_F1_LSB;
    index += 16;

    gps_eph.af0 = static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 22))) * A_F0_LSB;
    index += 22;

    gps_eph.IODC = static_cast<double>(Rtcm::bin_to_uint(message_bin.substr(index, 10)));
    index += 10;

    gps_eph.Crs = static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 16))) * C_RS_LSB;
    index += 16;

    gps_eph.delta_n = static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 16))) * DELTA_N_LSB;
    index += 16;

    gps_eph.M_0 = static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 32))) * M_0_LSB;
    index += 32;

    gps_eph.Cuc = static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 16))) * C_UC_LSB;
    index += 16;

    gps_eph.ecc = static_cast<double>(Rtcm::bin_to_uint(message_bin.substr(index, 32))) * ECCENTRICITY_LSB;
    index += 32;

    gps_eph.Cus = static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 16))) * C_US_LSB;
    index += 16;

    gps_eph.sqrtA = static_cast<double>(Rtcm::bin_to_uint(message_bin.substr(index, 32))) * SQRT_A_LSB;
    index += 32;

    gps_eph.toe = static_cast<double>(Rtcm::bin_to_uint(message_bin.substr(index, 16))) * T_OE_LSB;
    index += 16;

    gps_eph.Cic = static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 16))) * C_IC_LSB;
    index += 16;

    gps_eph.OMEGA_0 = static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 32))) * OMEGA_0_LSB;
    index += 32;

    gps_eph.Cis = static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 16))) * C_IS_LSB;
    index += 16;

    gps_eph.i_0 = static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 32))) * I_0_LSB;
    index += 32;

    gps_eph.Crc = static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 16))) * C_RC_LSB;
    index += 16;

    gps_eph.omega = static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 32))) * OMEGA_LSB;
    index += 32;

    gps_eph.OMEGAdot = static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 24))) * OMEGA_DOT_LSB;
    index += 24;

    gps_eph.TGD = static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 8))) * T_GD_LSB;
    index += 8;

    gps_eph.SV_health = static_cast<int32_t>(Rtcm::bin_to_uint(message_bin.substr(index, 6)));
    index += 6;

    gps_eph.L2_P_data_flag = static_cast<bool>(Rtcm::bin_to_uint(message_bin.substr(index, 1)));
    index += 1;

    gps_eph.fit_interval_flag = static_cast<bool>(Rtcm::bin_to_uint(message_bin.substr(index, 1)));

    return 0;
}


// ********************************************************
//
//   MESSAGE TYPE 1020 (GLONASS EPHEMERIS)
//
// ********************************************************

std::string Rtcm::print_MT1020(const Glonass_Gnav_Ephemeris& glonass_gnav_eph, const Glonass_Gnav_Utc_Model& glonass_gnav_utc_model)
{
    const uint32_t msg_number = 1020;
    const uint32_t glonass_gnav_alm_health = 0;
    const uint32_t glonass_gnav_alm_health_ind = 0;
    const uint32_t fifth_str_additional_data_ind = 1;

    Rtcm::set_DF002(msg_number);
    Rtcm::set_DF038(glonass_gnav_eph);
    Rtcm::set_DF040(glonass_gnav_eph);
    Rtcm::set_DF104(glonass_gnav_alm_health);
    Rtcm::set_DF105(glonass_gnav_alm_health_ind);
    Rtcm::set_DF106(glonass_gnav_eph);
    Rtcm::set_DF107(glonass_gnav_eph);
    Rtcm::set_DF108(glonass_gnav_eph);
    Rtcm::set_DF109(glonass_gnav_eph);
    Rtcm::set_DF110(glonass_gnav_eph);
    Rtcm::set_DF111(glonass_gnav_eph);
    Rtcm::set_DF112(glonass_gnav_eph);
    Rtcm::set_DF113(glonass_gnav_eph);
    Rtcm::set_DF114(glonass_gnav_eph);
    Rtcm::set_DF115(glonass_gnav_eph);
    Rtcm::set_DF116(glonass_gnav_eph);
    Rtcm::set_DF117(glonass_gnav_eph);
    Rtcm::set_DF118(glonass_gnav_eph);
    Rtcm::set_DF119(glonass_gnav_eph);
    Rtcm::set_DF120(glonass_gnav_eph);
    Rtcm::set_DF121(glonass_gnav_eph);
    Rtcm::set_DF122(glonass_gnav_eph);
    Rtcm::set_DF123(glonass_gnav_eph);
    Rtcm::set_DF124(glonass_gnav_eph);
    Rtcm::set_DF125(glonass_gnav_eph);
    Rtcm::set_DF126(glonass_gnav_eph);
    Rtcm::set_DF127(glonass_gnav_eph);
    Rtcm::set_DF128(glonass_gnav_eph);
    Rtcm::set_DF129(glonass_gnav_eph);
    Rtcm::set_DF130(glonass_gnav_eph);
    Rtcm::set_DF131(fifth_str_additional_data_ind);
    Rtcm::set_DF132(glonass_gnav_utc_model);
    Rtcm::set_DF133(glonass_gnav_utc_model);
    Rtcm::set_DF134(glonass_gnav_utc_model);
    Rtcm::set_DF135(glonass_gnav_utc_model);
    Rtcm::set_DF136(glonass_gnav_eph);

    const std::string data = DF002.to_string() +
                             DF038.to_string() +
                             DF040.to_string() +
                             DF104.to_string() +
                             DF105.to_string() +
                             DF106.to_string() +
                             DF107.to_string() +
                             DF108.to_string() +
                             DF109.to_string() +
                             DF110.to_string() +
                             DF111.to_string() +
                             DF112.to_string() +
                             DF113.to_string() +
                             DF114.to_string() +
                             DF115.to_string() +
                             DF116.to_string() +
                             DF117.to_string() +
                             DF118.to_string() +
                             DF119.to_string() +
                             DF120.to_string() +
                             DF121.to_string() +
                             DF122.to_string() +
                             DF123.to_string() +
                             DF124.to_string() +
                             DF125.to_string() +
                             DF126.to_string() +
                             DF127.to_string() +
                             DF128.to_string() +
                             DF129.to_string() +
                             DF130.to_string() +
                             DF131.to_string() +
                             DF132.to_string() +
                             DF133.to_string() +
                             DF134.to_string() +
                             DF135.to_string() +
                             DF136.to_string() +
                             std::bitset<7>().to_string();  // Reserved bits

    if (data.length() != 360)
        {
            LOG(WARNING) << "Bad-formatted RTCM MT1020 (360 bits expected, found " << data.length() << ")";
        }

    std::string msg = build_message(data);
    if (server_is_running)
        {
            rtcm_message_queue->push(msg);
        }
    return msg;
}


int32_t Rtcm::read_MT1020(const std::string& message, Glonass_Gnav_Ephemeris& glonass_gnav_eph, Glonass_Gnav_Utc_Model& glonass_gnav_utc_model) const
{
    // Convert message to binary
    const std::string message_bin = Rtcm::binary_data_to_bin(message);
    int32_t glonass_gnav_alm_health = 0;
    int32_t glonass_gnav_alm_health_ind = 0;
    int32_t fifth_str_additional_data_ind = 0;

    if (!Rtcm::check_CRC(message))
        {
            LOG(WARNING) << " Bad CRC detected in RTCM message MT1020";
            return 1;
        }

    const uint32_t preamble_length = 8;
    const uint32_t reserved_field_length = 6;
    uint32_t index = preamble_length + reserved_field_length;

    const uint32_t read_message_length = Rtcm::bin_to_uint(message_bin.substr(index, 10));
    index += 10;

    if (read_message_length != 45)  // 360 bits = 45 bytes
        {
            LOG(WARNING) << " Message MT1020 seems too long (61 bytes expected, " << read_message_length << " received)";
            return 1;
        }

    // Check than the message number is correct
    const uint32_t read_msg_number = Rtcm::bin_to_uint(message_bin.substr(index, 12));
    index += 12;

    if (1020 != read_msg_number)
        {
            LOG(WARNING) << " This is not a MT1020 message";
            return 1;
        }

    // Fill Gps Ephemeris with message data content
    glonass_gnav_eph.i_satellite_slot_number = Rtcm::bin_to_uint(message_bin.substr(index, 6));
    index += 6;

    glonass_gnav_eph.i_satellite_freq_channel = static_cast<int32_t>(Rtcm::bin_to_uint(message_bin.substr(index, 5)) - 7.0);
    index += 5;

    glonass_gnav_alm_health = static_cast<int32_t>(Rtcm::bin_to_uint(message_bin.substr(index, 1)));
    index += 1;
    if (glonass_gnav_alm_health)
        {
        }  // Avoid compiler warning

    glonass_gnav_alm_health_ind = static_cast<int32_t>(Rtcm::bin_to_uint(message_bin.substr(index, 1)));
    index += 1;
    if (glonass_gnav_alm_health_ind)
        {
        }  // Avoid compiler warning

    uint32_t P_1_tmp = Rtcm::bin_to_uint(message_bin.substr(index, 2));
    glonass_gnav_eph.d_P_1 = (P_1_tmp == 0) ? 0. : (P_1_tmp + 1) * 15;
    index += 2;

    glonass_gnav_eph.d_t_k += static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 5))) * 3600;
    index += 5;
    glonass_gnav_eph.d_t_k += static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 6))) * 60;
    index += 6;
    glonass_gnav_eph.d_t_k += static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 1))) * 30;
    index += 1;

    glonass_gnav_eph.d_B_n = static_cast<double>(Rtcm::bin_to_uint(message_bin.substr(index, 1)));
    index += 1;

    glonass_gnav_eph.d_P_2 = static_cast<bool>(Rtcm::bin_to_uint(message_bin.substr(index, 1)));
    index += 1;

    glonass_gnav_eph.d_t_b = static_cast<double>(Rtcm::bin_to_uint(message_bin.substr(index, 7))) * 15 * 60.0;
    index += 7;

    // TODO Check for type spec for intS24
    glonass_gnav_eph.d_VXn = static_cast<double>(Rtcm::bin_to_sint(message_bin.substr(index, 24))) * TWO_N20;
    index += 24;

    glonass_gnav_eph.d_Xn = static_cast<double>(Rtcm::bin_to_sint(message_bin.substr(index, 27))) * TWO_N11;
    index += 27;

    glonass_gnav_eph.d_AXn = static_cast<double>(Rtcm::bin_to_sint(message_bin.substr(index, 5))) * TWO_N30;
    index += 5;

    glonass_gnav_eph.d_VYn = static_cast<double>(Rtcm::bin_to_sint(message_bin.substr(index, 24))) * TWO_N20;
    index += 24;

    glonass_gnav_eph.d_Yn = static_cast<double>(Rtcm::bin_to_sint(message_bin.substr(index, 27))) * TWO_N11;
    index += 27;

    glonass_gnav_eph.d_AYn = static_cast<double>(Rtcm::bin_to_sint(message_bin.substr(index, 5))) * TWO_N30;
    index += 5;

    glonass_gnav_eph.d_VZn = static_cast<double>(Rtcm::bin_to_sint(message_bin.substr(index, 24))) * TWO_N20;
    index += 24;

    glonass_gnav_eph.d_Zn = static_cast<double>(Rtcm::bin_to_sint(message_bin.substr(index, 27))) * TWO_N11;
    index += 27;

    glonass_gnav_eph.d_AZn = static_cast<double>(Rtcm::bin_to_sint(message_bin.substr(index, 5))) * TWO_N30;
    index += 5;

    glonass_gnav_eph.d_P_3 = static_cast<bool>(Rtcm::bin_to_uint(message_bin.substr(index, 1)));
    index += 1;

    glonass_gnav_eph.d_gamma_n = static_cast<double>(Rtcm::bin_to_sint(message_bin.substr(index, 11))) * TWO_N30;
    index += 11;

    glonass_gnav_eph.d_P = static_cast<double>(Rtcm::bin_to_uint(message_bin.substr(index, 2)));
    index += 2;

    glonass_gnav_eph.d_l3rd_n = static_cast<bool>(Rtcm::bin_to_uint(message_bin.substr(index, 1)));
    index += 1;

    glonass_gnav_eph.d_tau_n = static_cast<double>(Rtcm::bin_to_sint(message_bin.substr(index, 22))) * TWO_N30;
    index += 22;

    glonass_gnav_eph.d_Delta_tau_n = static_cast<double>(Rtcm::bin_to_sint(message_bin.substr(index, 5))) * TWO_N30;
    index += 5;

    glonass_gnav_eph.d_E_n = static_cast<double>(Rtcm::bin_to_uint(message_bin.substr(index, 5)));
    index += 5;

    glonass_gnav_eph.d_P_4 = static_cast<bool>(Rtcm::bin_to_uint(message_bin.substr(index, 1)));
    index += 1;

    glonass_gnav_eph.d_F_T = static_cast<double>(Rtcm::bin_to_uint(message_bin.substr(index, 4)));
    index += 4;

    glonass_gnav_eph.d_N_T = static_cast<double>(Rtcm::bin_to_uint(message_bin.substr(index, 11)));
    index += 11;

    glonass_gnav_eph.d_M = static_cast<double>(Rtcm::bin_to_uint(message_bin.substr(index, 2)));
    index += 2;

    fifth_str_additional_data_ind = static_cast<double>(Rtcm::bin_to_uint(message_bin.substr(index, 1)));
    index += 1;

    if (fifth_str_additional_data_ind == true)
        {
            glonass_gnav_utc_model.d_N_A = static_cast<double>(Rtcm::bin_to_uint(message_bin.substr(index, 11)));
            index += 11;

            glonass_gnav_utc_model.d_tau_c = static_cast<double>(Rtcm::bin_to_sint(message_bin.substr(index, 32))) * TWO_N31;
            index += 32;

            glonass_gnav_utc_model.d_N_4 = static_cast<double>(Rtcm::bin_to_uint(message_bin.substr(index, 5)));
            index += 5;

            glonass_gnav_utc_model.d_tau_gps = static_cast<double>(Rtcm::bin_to_sint(message_bin.substr(index, 22))) * TWO_N30;
            index += 22;

            glonass_gnav_eph.d_l5th_n = static_cast<int32_t>(Rtcm::bin_to_uint(message_bin.substr(index, 1)));
        }

    return 0;
}


// ********************************************************
//
//   MESSAGE TYPE 1029 (UNICODE TEXT STRING)
//
// ********************************************************

std::string Rtcm::print_MT1029(uint32_t ref_id, const Gps_Ephemeris& gps_eph, double obs_time, const std::string& message)
{
    const uint32_t msg_number = 1029;

    Rtcm::set_DF002(msg_number);
    Rtcm::set_DF003(ref_id);
    Rtcm::set_DF051(gps_eph, obs_time);
    Rtcm::set_DF052(gps_eph, obs_time);

    uint32_t i = 0;
    bool first = true;
    std::string text_binary;
    for (char c : message)
        {
            if (isgraph(c) || c == ' ')
                {
                    i++;
                    first = true;
                }
            else
                {
                    if (!first)
                        {
                            i++;
                            first = true;
                        }
                    else
                        {
                            first = false;
                        }
                }
            const auto character = std::bitset<8>(c);
            text_binary += character.to_string();
        }

    const auto DF138_ = std::bitset<7>(i);
    const auto DF139_ = std::bitset<8>(message.length());

    const std::string data = DF002.to_string() +
                             DF003.to_string() +
                             DF051.to_string() +
                             DF052.to_string() +
                             DF138_.to_string() +
                             DF139_.to_string() +
                             text_binary;

    std::string msg = build_message(data);
    if (server_is_running)
        {
            rtcm_message_queue->push(msg);
        }
    return msg;
}


// ********************************************************
//
//   MESSAGE TYPE 1045 (GALILEO EPHEMERIS)
//
// ********************************************************

std::string Rtcm::print_MT1045(const Galileo_Ephemeris& gal_eph)
{
    const uint32_t msg_number = 1045;

    Rtcm::set_DF002(msg_number);
    Rtcm::set_DF252(gal_eph);
    Rtcm::set_DF289(gal_eph);
    Rtcm::set_DF290(gal_eph);
    Rtcm::set_DF291(gal_eph);
    Rtcm::set_DF293(gal_eph);
    Rtcm::set_DF294(gal_eph);
    Rtcm::set_DF295(gal_eph);
    Rtcm::set_DF296(gal_eph);
    Rtcm::set_DF297(gal_eph);
    Rtcm::set_DF298(gal_eph);
    Rtcm::set_DF299(gal_eph);
    Rtcm::set_DF300(gal_eph);
    Rtcm::set_DF301(gal_eph);
    Rtcm::set_DF302(gal_eph);
    Rtcm::set_DF303(gal_eph);
    Rtcm::set_DF304(gal_eph);
    Rtcm::set_DF305(gal_eph);
    Rtcm::set_DF306(gal_eph);
    Rtcm::set_DF307(gal_eph);
    Rtcm::set_DF308(gal_eph);
    Rtcm::set_DF309(gal_eph);
    Rtcm::set_DF310(gal_eph);
    Rtcm::set_DF311(gal_eph);
    Rtcm::set_DF312(gal_eph);
    Rtcm::set_DF314(gal_eph);
    Rtcm::set_DF315(gal_eph);
    const uint32_t seven_zero = 0;
    const auto DF001_ = std::bitset<7>(seven_zero);

    const std::string data = DF002.to_string() +
                             DF252.to_string() +
                             DF289.to_string() +
                             DF290.to_string() +
                             DF291.to_string() +
                             DF292.to_string() +
                             DF293.to_string() +
                             DF294.to_string() +
                             DF295.to_string() +
                             DF296.to_string() +
                             DF297.to_string() +
                             DF298.to_string() +
                             DF299.to_string() +
                             DF300.to_string() +
                             DF301.to_string() +
                             DF302.to_string() +
                             DF303.to_string() +
                             DF304.to_string() +
                             DF305.to_string() +
                             DF306.to_string() +
                             DF307.to_string() +
                             DF308.to_string() +
                             DF309.to_string() +
                             DF310.to_string() +
                             DF311.to_string() +
                             DF312.to_string() +
                             DF314.to_string() +
                             DF315.to_string() +
                             DF001_.to_string();

    if (data.length() != 496)
        {
            LOG(WARNING) << "Bad-formatted RTCM MT1045 (496 bits expected, found " << data.length() << ")";
        }

    std::string msg = build_message(data);
    if (server_is_running)
        {
            rtcm_message_queue->push(msg);
        }
    return msg;
}


int32_t Rtcm::read_MT1045(const std::string& message, Galileo_Ephemeris& gal_eph) const
{
    // Convert message to binary
    const std::string message_bin = Rtcm::binary_data_to_bin(message);

    if (!Rtcm::check_CRC(message))
        {
            LOG(WARNING) << " Bad CRC detected in RTCM message MT1045";
            return 1;
        }

    const uint32_t preamble_length = 8;
    const uint32_t reserved_field_length = 6;
    uint32_t index = preamble_length + reserved_field_length;

    const uint32_t read_message_length = Rtcm::bin_to_uint(message_bin.substr(index, 10));
    index += 10;

    if (read_message_length != 62)
        {
            LOG(WARNING) << " Message MT1045 seems too long (62 bytes expected, " << read_message_length << " received)";
            return 1;
        }

    // Check than the message number is correct
    const uint32_t read_msg_number = Rtcm::bin_to_uint(message_bin.substr(index, 12));
    index += 12;

    if (1045 != read_msg_number)
        {
            LOG(WARNING) << " This is not a MT1045 message";
            return 1;
        }

    // Fill Galileo Ephemeris with message data content
    gal_eph.PRN = Rtcm::bin_to_uint(message_bin.substr(index, 6));
    index += 6;

    gal_eph.WN = static_cast<double>(Rtcm::bin_to_uint(message_bin.substr(index, 12)));
    index += 12;

    gal_eph.IOD_nav = static_cast<int32_t>(Rtcm::bin_to_uint(message_bin.substr(index, 10)));
    index += 10;

    gal_eph.SISA = static_cast<double>(Rtcm::bin_to_uint(message_bin.substr(index, 8)));
    index += 8;

    gal_eph.idot = static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 14))) * I_DOT_2_LSB;
    index += 14;

    gal_eph.toc = static_cast<double>(Rtcm::bin_to_uint(message_bin.substr(index, 14))) * T0C_4_LSB;
    index += 14;

    gal_eph.af2 = static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 6))) * AF2_4_LSB;
    index += 6;

    gal_eph.af1 = static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 21))) * AF1_4_LSB;
    index += 21;

    gal_eph.af0 = static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 31))) * AF0_4_LSB;
    index += 31;

    gal_eph.Crs = static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 16))) * C_RS_3_LSB;
    index += 16;

    gal_eph.delta_n = static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 16))) * DELTA_N_3_LSB;
    index += 16;

    gal_eph.M_0 = static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 32))) * M0_1_LSB;
    index += 32;

    gal_eph.Cuc = static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 16))) * C_UC_3_LSB;
    index += 16;

    gal_eph.ecc = static_cast<double>(Rtcm::bin_to_uint(message_bin.substr(index, 32))) * E_1_LSB;
    index += 32;

    gal_eph.Cus = static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 16))) * C_US_3_LSB;
    index += 16;

    gal_eph.sqrtA = static_cast<double>(Rtcm::bin_to_uint(message_bin.substr(index, 32))) * A_1_LSB_GAL;
    index += 32;

    gal_eph.toe = static_cast<double>(Rtcm::bin_to_uint(message_bin.substr(index, 14))) * T0E_1_LSB;
    index += 14;

    gal_eph.Cic = static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 16))) * C_IC_4_LSB;
    index += 16;

    gal_eph.OMEGA_0 = static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 32))) * OMEGA_0_2_LSB;
    index += 32;

    gal_eph.Cis = static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 16))) * C_IS_4_LSB;
    index += 16;

    gal_eph.i_0 = static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 32))) * I_0_2_LSB;
    index += 32;

    gal_eph.Crc = static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 16))) * C_RC_3_LSB;
    index += 16;

    gal_eph.omega = static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 32))) * OMEGA_2_LSB;
    index += 32;

    gal_eph.OMEGAdot = static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 24))) * OMEGA_DOT_3_LSB;
    index += 24;

    gal_eph.BGD_E1E5a = static_cast<double>(Rtcm::bin_to_int(message_bin.substr(index, 10)));
    index += 10;

    gal_eph.E5a_HS = Rtcm::bin_to_uint(message_bin.substr(index, 2));
    index += 2;

    gal_eph.E5a_DVS = static_cast<bool>(Rtcm::bin_to_uint(message_bin.substr(index, 1)));

    return 0;
}


// **********************************************************************************************
//
//   MESSAGE TYPE MSM1 (COMPACT observables)
//
// **********************************************************************************************

std::string Rtcm::print_MSM_1(const Gps_Ephemeris& gps_eph,
    const Gps_CNAV_Ephemeris& gps_cnav_eph,
    const Galileo_Ephemeris& gal_eph,
    const Glonass_Gnav_Ephemeris& glo_gnav_eph,
    double obs_time,
    const std::map<int32_t, Gnss_Synchro>& observables,
    uint32_t ref_id,
    uint32_t clock_steering_indicator,
    uint32_t external_clock_indicator,
    int32_t smooth_int,
    bool divergence_free,
    bool more_messages)
{
    uint32_t msg_number = 0;
    if (gps_eph.PRN != 0)
        {
            msg_number = 1071;
        }
    if (gps_cnav_eph.PRN != 0)
        {
            msg_number = 1071;
        }
    if (glo_gnav_eph.PRN != 0)
        {
            msg_number = 1081;
        }
    if (gal_eph.PRN != 0)
        {
            msg_number = 1091;
        }
    if (((gps_eph.PRN != 0) || (gps_cnav_eph.PRN != 0)) && (gal_eph.PRN != 0) && (glo_gnav_eph.PRN != 0))
        {
            LOG(WARNING) << "MSM messages for observables from different systems are not defined";  // print two messages?
        }
    if (msg_number == 0)
        {
            LOG(WARNING) << "Invalid ephemeris provided";
            msg_number = 1071;
        }

    const std::string header = Rtcm::get_MSM_header(msg_number,
        obs_time,
        observables,
        ref_id,
        clock_steering_indicator,
        external_clock_indicator,
        smooth_int,
        divergence_free,
        more_messages);

    const std::string sat_data = Rtcm::get_MSM_1_content_sat_data(observables);

    const std::string signal_data = Rtcm::get_MSM_1_content_signal_data(observables);

    std::string message = build_message(header + sat_data + signal_data);

    if (server_is_running)
        {
            rtcm_message_queue->push(message);
        }

    return message;
}


std::string Rtcm::get_MSM_header(uint32_t msg_number,
    double obs_time,
    const std::map<int32_t, Gnss_Synchro>& observables,
    uint32_t ref_id,
    uint32_t clock_steering_indicator,
    uint32_t external_clock_indicator,
    int32_t smooth_int,
    bool divergence_free,
    bool more_messages)
{
    // Find first element in observables block and define type of message
    auto observables_iter = observables.begin();
    const std::string sys(observables_iter->second.System, 1);

    Rtcm::set_DF002(msg_number);
    Rtcm::set_DF003(ref_id);
    Rtcm::set_DF393(more_messages);
    Rtcm::set_DF409(0);  // Issue of Data Station. 0: not utilized
    const auto DF001_ = std::bitset<7>("0000000");
    Rtcm::set_DF411(clock_steering_indicator);
    Rtcm::set_DF412(external_clock_indicator);
    Rtcm::set_DF417(divergence_free);
    Rtcm::set_DF418(smooth_int);

    Rtcm::set_DF394(observables);
    Rtcm::set_DF395(observables);

    std::string header = DF002.to_string() + DF003.to_string();
    // GNSS Epoch Time Specific to each constellation
    if ((sys == "R"))
        {
            // GLONASS Epoch Time
            Rtcm::set_DF034(obs_time);
            header += DF034.to_string();
        }
    else
        {
            // GPS, Galileo Epoch Time
            Rtcm::set_DF004(obs_time);
            header += DF004.to_string();
        }

    header = header + DF393.to_string() +
             DF409.to_string() +
             DF001_.to_string() +
             DF411.to_string() +
             DF417.to_string() +
             DF412.to_string() +
             DF418.to_string() +
             DF394.to_string() +
             DF395.to_string() +
             Rtcm::set_DF396(observables);

    return header;
}


std::string Rtcm::get_MSM_1_content_sat_data(const std::map<int32_t, Gnss_Synchro>& observables)
{
    std::string sat_data;

    Rtcm::set_DF394(observables);
    const uint32_t num_satellites = DF394.count();
    const uint32_t numobs = observables.size();

    auto observables_vector = std::vector<std::pair<int32_t, Gnss_Synchro>>();

    observables_vector.reserve(numobs);
    std::map<int32_t, Gnss_Synchro>::const_iterator gnss_synchro_iter;
    auto pos = std::vector<uint32_t>();
    pos.reserve(numobs);
    std::vector<uint32_t>::iterator it;

    for (gnss_synchro_iter = observables.cbegin();
        gnss_synchro_iter != observables.cend();
        gnss_synchro_iter++)
        {
            it = std::find(pos.begin(), pos.end(), 65 - gnss_synchro_iter->second.PRN);
            if (it == pos.end())
                {
                    pos.push_back(65 - gnss_synchro_iter->second.PRN);
                    observables_vector.emplace_back(*gnss_synchro_iter);
                }
        }

    const std::vector<std::pair<int32_t, Gnss_Synchro>> ordered_by_PRN_pos = Rtcm::sort_by_PRN_mask(observables_vector);

    for (uint32_t nsat = 0; nsat < num_satellites; nsat++)
        {
            Rtcm::set_DF398(ordered_by_PRN_pos.at(nsat).second);
            sat_data += DF398.to_string();
        }

    return sat_data;
}


std::string Rtcm::get_MSM_1_content_signal_data(const std::map<int32_t, Gnss_Synchro>& observables)
{
    std::string signal_data;
    const uint32_t Ncells = observables.size();

    auto observables_vector = std::vector<std::pair<int32_t, Gnss_Synchro>>();
    observables_vector.reserve(Ncells);
    std::map<int32_t, Gnss_Synchro>::const_iterator map_iter;

    for (map_iter = observables.cbegin();
        map_iter != observables.cend();
        map_iter++)
        {
            observables_vector.emplace_back(*map_iter);
        }

    std::vector<std::pair<int32_t, Gnss_Synchro>> ordered_by_signal = Rtcm::sort_by_signal(observables_vector);
    std::reverse(ordered_by_signal.begin(), ordered_by_signal.end());
    const std::vector<std::pair<int32_t, Gnss_Synchro>> ordered_by_PRN_pos = Rtcm::sort_by_PRN_mask(ordered_by_signal);

    for (uint32_t cell = 0; cell < Ncells; cell++)
        {
            Rtcm::set_DF400(ordered_by_PRN_pos.at(cell).second);
            signal_data += DF400.to_string();
        }

    return signal_data;
}


// **********************************************************************************************
//
//   MESSAGE TYPE MSM2 (COMPACT PHASERANGES)
//
// **********************************************************************************************

std::string Rtcm::print_MSM_2(const Gps_Ephemeris& gps_eph,
    const Gps_CNAV_Ephemeris& gps_cnav_eph,
    const Galileo_Ephemeris& gal_eph,
    const Glonass_Gnav_Ephemeris& glo_gnav_eph,
    double obs_time,
    const std::map<int32_t, Gnss_Synchro>& observables,
    uint32_t ref_id,
    uint32_t clock_steering_indicator,
    uint32_t external_clock_indicator,
    int32_t smooth_int,
    bool divergence_free,
    bool more_messages)
{
    uint32_t msg_number = 0;
    if (gps_eph.PRN != 0)
        {
            msg_number = 1072;
        }
    if (gps_cnav_eph.PRN != 0)
        {
            msg_number = 1072;
        }
    if (glo_gnav_eph.PRN != 0)
        {
            msg_number = 1082;
        }
    if (gal_eph.PRN != 0)
        {
            msg_number = 1092;
        }
    if (((gps_eph.PRN != 0) || (gps_cnav_eph.PRN != 0)) && (gal_eph.PRN != 0) && (glo_gnav_eph.PRN != 0))
        {
            LOG(WARNING) << "MSM messages for observables from different systems are not defined";  // print two messages?
        }
    if (msg_number == 0)
        {
            LOG(WARNING) << "Invalid ephemeris provided";
            msg_number = 1072;
        }

    const std::string header = Rtcm::get_MSM_header(msg_number,
        obs_time,
        observables,
        ref_id,
        clock_steering_indicator,
        external_clock_indicator,
        smooth_int,
        divergence_free,
        more_messages);

    const std::string sat_data = Rtcm::get_MSM_1_content_sat_data(observables);

    const std::string signal_data = Rtcm::get_MSM_2_content_signal_data(gps_eph, gps_cnav_eph, gal_eph, glo_gnav_eph, obs_time, observables);

    std::string message = build_message(header + sat_data + signal_data);
    if (server_is_running)
        {
            rtcm_message_queue->push(message);
        }

    return message;
}


std::string Rtcm::get_MSM_2_content_signal_data(const Gps_Ephemeris& ephNAV,
    const Gps_CNAV_Ephemeris& ephCNAV,
    const Galileo_Ephemeris& ephFNAV,
    const Glonass_Gnav_Ephemeris& ephGNAV,
    double obs_time,
    const std::map<int32_t, Gnss_Synchro>& observables)
{
    std::string signal_data;
    std::string first_data_type;
    std::string second_data_type;
    std::string third_data_type;

    const uint32_t Ncells = observables.size();

    auto observables_vector = std::vector<std::pair<int32_t, Gnss_Synchro>>();
    observables_vector.reserve(Ncells);
    std::map<int32_t, Gnss_Synchro>::const_iterator map_iter;

    for (map_iter = observables.cbegin();
        map_iter != observables.cend();
        map_iter++)
        {
            observables_vector.emplace_back(*map_iter);
        }

    std::vector<std::pair<int32_t, Gnss_Synchro>> ordered_by_signal = Rtcm::sort_by_signal(observables_vector);
    std::reverse(ordered_by_signal.begin(), ordered_by_signal.end());
    const std::vector<std::pair<int32_t, Gnss_Synchro>> ordered_by_PRN_pos = Rtcm::sort_by_PRN_mask(ordered_by_signal);

    for (uint32_t cell = 0; cell < Ncells; cell++)
        {
            Rtcm::set_DF401(ordered_by_PRN_pos.at(cell).second);
            Rtcm::set_DF402(ephNAV, ephCNAV, ephFNAV, ephGNAV, obs_time, ordered_by_PRN_pos.at(cell).second);
            Rtcm::set_DF420(ordered_by_PRN_pos.at(cell).second);
            first_data_type += DF401.to_string();
            second_data_type += DF402.to_string();
            third_data_type += DF420.to_string();
        }

    signal_data = first_data_type + second_data_type + third_data_type;
    return signal_data;
}


// **********************************************************************************************
//
//   MESSAGE TYPE MSM3 (COMPACT PSEUDORANGES AND PHASERANGES)
//
// **********************************************************************************************

std::string Rtcm::print_MSM_3(const Gps_Ephemeris& gps_eph,
    const Gps_CNAV_Ephemeris& gps_cnav_eph,
    const Galileo_Ephemeris& gal_eph,
    const Glonass_Gnav_Ephemeris& glo_gnav_eph,
    double obs_time,
    const std::map<int32_t, Gnss_Synchro>& observables,
    uint32_t ref_id,
    uint32_t clock_steering_indicator,
    uint32_t external_clock_indicator,
    int32_t smooth_int,
    bool divergence_free,
    bool more_messages)
{
    uint32_t msg_number = 0;
    if (gps_eph.PRN != 0)
        {
            msg_number = 1073;
        }
    if (gps_cnav_eph.PRN != 0)
        {
            msg_number = 1073;
        }
    if (glo_gnav_eph.PRN != 0)
        {
            msg_number = 1083;
        }
    if (gal_eph.PRN != 0)
        {
            msg_number = 1093;
        }
    if (((gps_eph.PRN != 0) || (gps_cnav_eph.PRN != 0)) && (gal_eph.PRN != 0) && (glo_gnav_eph.PRN != 0))
        {
            LOG(WARNING) << "MSM messages for observables from different systems are not defined";  // print two messages?
        }
    if (msg_number == 0)
        {
            LOG(WARNING) << "Invalid ephemeris provided";
            msg_number = 1073;
        }

    const std::string header = Rtcm::get_MSM_header(msg_number,
        obs_time,
        observables,
        ref_id,
        clock_steering_indicator,
        external_clock_indicator,
        smooth_int,
        divergence_free,
        more_messages);

    const std::string sat_data = Rtcm::get_MSM_1_content_sat_data(observables);

    const std::string signal_data = Rtcm::get_MSM_3_content_signal_data(gps_eph, gps_cnav_eph, gal_eph, glo_gnav_eph, obs_time, observables);

    std::string message = build_message(header + sat_data + signal_data);
    if (server_is_running)
        {
            rtcm_message_queue->push(message);
        }

    return message;
}


std::string Rtcm::get_MSM_3_content_signal_data(const Gps_Ephemeris& ephNAV,
    const Gps_CNAV_Ephemeris& ephCNAV,
    const Galileo_Ephemeris& ephFNAV,
    const Glonass_Gnav_Ephemeris& ephGNAV,
    double obs_time,
    const std::map<int32_t, Gnss_Synchro>& observables)
{
    std::string signal_data;
    std::string first_data_type;
    std::string second_data_type;
    std::string third_data_type;
    std::string fourth_data_type;

    const uint32_t Ncells = observables.size();

    auto observables_vector = std::vector<std::pair<int32_t, Gnss_Synchro>>();
    observables_vector.reserve(Ncells);
    std::map<int32_t, Gnss_Synchro>::const_iterator map_iter;

    for (map_iter = observables.cbegin();
        map_iter != observables.cend();
        map_iter++)
        {
            observables_vector.emplace_back(*map_iter);
        }

    std::vector<std::pair<int32_t, Gnss_Synchro>> ordered_by_signal = Rtcm::sort_by_signal(observables_vector);
    std::reverse(ordered_by_signal.begin(), ordered_by_signal.end());
    const std::vector<std::pair<int32_t, Gnss_Synchro>> ordered_by_PRN_pos = Rtcm::sort_by_PRN_mask(ordered_by_signal);

    for (uint32_t cell = 0; cell < Ncells; cell++)
        {
            Rtcm::set_DF400(ordered_by_PRN_pos.at(cell).second);
            Rtcm::set_DF401(ordered_by_PRN_pos.at(cell).second);
            Rtcm::set_DF402(ephNAV, ephCNAV, ephFNAV, ephGNAV, obs_time, ordered_by_PRN_pos.at(cell).second);
            Rtcm::set_DF420(ordered_by_PRN_pos.at(cell).second);
            first_data_type += DF400.to_string();
            second_data_type += DF401.to_string();
            third_data_type += DF402.to_string();
            fourth_data_type += DF420.to_string();
        }

    signal_data = first_data_type + second_data_type + third_data_type + fourth_data_type;
    return signal_data;
}


// **********************************************************************************************
//
//   MESSAGE TYPE MSM4 (FULL PSEUDORANGES AND PHASERANGES PLUS CNR)
//
// **********************************************************************************************

std::string Rtcm::print_MSM_4(const Gps_Ephemeris& gps_eph,
    const Gps_CNAV_Ephemeris& gps_cnav_eph,
    const Galileo_Ephemeris& gal_eph,
    const Glonass_Gnav_Ephemeris& glo_gnav_eph,
    double obs_time,
    const std::map<int32_t, Gnss_Synchro>& observables,
    uint32_t ref_id,
    uint32_t clock_steering_indicator,
    uint32_t external_clock_indicator,
    int32_t smooth_int,
    bool divergence_free,
    bool more_messages)
{
    uint32_t msg_number = 0;
    if (gps_eph.PRN != 0)
        {
            msg_number = 1074;
        }
    if (gps_cnav_eph.PRN != 0)
        {
            msg_number = 1074;
        }
    if (glo_gnav_eph.PRN != 0)
        {
            msg_number = 1084;
        }
    if (gal_eph.PRN != 0)
        {
            msg_number = 1094;
        }
    if (((gps_eph.PRN != 0) || (gps_cnav_eph.PRN != 0)) && (gal_eph.PRN != 0) && (glo_gnav_eph.PRN != 0))
        {
            LOG(WARNING) << "MSM messages for observables from different systems are not defined";  // print two messages?
        }
    if (msg_number == 0)
        {
            LOG(WARNING) << "Invalid ephemeris provided";
            msg_number = 1074;
        }

    const std::string header = Rtcm::get_MSM_header(msg_number,
        obs_time,
        observables,
        ref_id,
        clock_steering_indicator,
        external_clock_indicator,
        smooth_int,
        divergence_free,
        more_messages);

    const std::string sat_data = Rtcm::get_MSM_4_content_sat_data(observables);

    const std::string signal_data = Rtcm::get_MSM_4_content_signal_data(gps_eph, gps_cnav_eph, gal_eph, glo_gnav_eph, obs_time, observables);

    std::string message = build_message(header + sat_data + signal_data);
    if (server_is_running)
        {
            rtcm_message_queue->push(message);
        }

    return message;
}


std::string Rtcm::get_MSM_4_content_sat_data(const std::map<int32_t, Gnss_Synchro>& observables)
{
    std::string sat_data;
    std::string first_data_type;
    std::string second_data_type;

    Rtcm::set_DF394(observables);
    const uint32_t num_satellites = DF394.count();
    const uint32_t numobs = observables.size();

    auto observables_vector = std::vector<std::pair<int32_t, Gnss_Synchro>>();
    observables_vector.reserve(numobs);
    std::map<int32_t, Gnss_Synchro>::const_iterator gnss_synchro_iter;
    auto pos = std::vector<uint32_t>();
    pos.reserve(numobs);
    std::vector<uint32_t>::iterator it;

    for (gnss_synchro_iter = observables.cbegin();
        gnss_synchro_iter != observables.cend();
        gnss_synchro_iter++)
        {
            it = std::find(pos.begin(), pos.end(), 65 - gnss_synchro_iter->second.PRN);
            if (it == pos.end())
                {
                    pos.push_back(65 - gnss_synchro_iter->second.PRN);
                    observables_vector.emplace_back(*gnss_synchro_iter);
                }
        }

    const std::vector<std::pair<int32_t, Gnss_Synchro>> ordered_by_PRN_pos = Rtcm::sort_by_PRN_mask(observables_vector);

    for (uint32_t nsat = 0; nsat < num_satellites; nsat++)
        {
            Rtcm::set_DF397(ordered_by_PRN_pos.at(nsat).second);
            Rtcm::set_DF398(ordered_by_PRN_pos.at(nsat).second);
            first_data_type += DF397.to_string();
            second_data_type += DF398.to_string();
        }
    sat_data = first_data_type + second_data_type;
    return sat_data;
}


std::string Rtcm::get_MSM_4_content_signal_data(const Gps_Ephemeris& ephNAV,
    const Gps_CNAV_Ephemeris& ephCNAV,
    const Galileo_Ephemeris& ephFNAV,
    const Glonass_Gnav_Ephemeris& ephGNAV,
    double obs_time,
    const std::map<int32_t, Gnss_Synchro>& observables)
{
    std::string signal_data;
    std::string first_data_type;
    std::string second_data_type;
    std::string third_data_type;
    std::string fourth_data_type;
    std::string fifth_data_type;

    const uint32_t Ncells = observables.size();

    auto observables_vector = std::vector<std::pair<int32_t, Gnss_Synchro>>();
    observables_vector.reserve(Ncells);
    std::map<int32_t, Gnss_Synchro>::const_iterator map_iter;

    for (map_iter = observables.cbegin();
        map_iter != observables.cend();
        map_iter++)
        {
            observables_vector.emplace_back(*map_iter);
        }

    std::vector<std::pair<int32_t, Gnss_Synchro>> ordered_by_signal = Rtcm::sort_by_signal(observables_vector);
    std::reverse(ordered_by_signal.begin(), ordered_by_signal.end());
    const std::vector<std::pair<int32_t, Gnss_Synchro>> ordered_by_PRN_pos = Rtcm::sort_by_PRN_mask(ordered_by_signal);

    for (uint32_t cell = 0; cell < Ncells; cell++)
        {
            Rtcm::set_DF400(ordered_by_PRN_pos.at(cell).second);
            Rtcm::set_DF401(ordered_by_PRN_pos.at(cell).second);
            Rtcm::set_DF402(ephNAV, ephCNAV, ephFNAV, ephGNAV, obs_time, ordered_by_PRN_pos.at(cell).second);
            Rtcm::set_DF420(ordered_by_PRN_pos.at(cell).second);
            Rtcm::set_DF403(ordered_by_PRN_pos.at(cell).second);
            first_data_type += DF400.to_string();
            second_data_type += DF401.to_string();
            third_data_type += DF402.to_string();
            fourth_data_type += DF420.to_string();
            fifth_data_type += DF403.to_string();
        }

    signal_data = first_data_type + second_data_type + third_data_type + fourth_data_type + fifth_data_type;
    return signal_data;
}


// **********************************************************************************************
//
//   MESSAGE TYPE MSM5 (FULL PSEUDORANGES, PHASERANGES, PHASERANGERATE PLUS CNR)
//
// **********************************************************************************************

std::string Rtcm::print_MSM_5(const Gps_Ephemeris& gps_eph,
    const Gps_CNAV_Ephemeris& gps_cnav_eph,
    const Galileo_Ephemeris& gal_eph,
    const Glonass_Gnav_Ephemeris& glo_gnav_eph,
    double obs_time,
    const std::map<int32_t, Gnss_Synchro>& observables,
    uint32_t ref_id,
    uint32_t clock_steering_indicator,
    uint32_t external_clock_indicator,
    int32_t smooth_int,
    bool divergence_free,
    bool more_messages)
{
    uint32_t msg_number = 0;
    if (gps_eph.PRN != 0)
        {
            msg_number = 1075;
        }
    if (gps_cnav_eph.PRN != 0)
        {
            msg_number = 1075;
        }
    if (glo_gnav_eph.PRN != 0)
        {
            msg_number = 1085;
        }
    if (gal_eph.PRN != 0)
        {
            msg_number = 1095;
        }
    if (((gps_eph.PRN != 0) || (gps_cnav_eph.PRN != 0)) && (gal_eph.PRN != 0) && (glo_gnav_eph.PRN != 0))
        {
            LOG(WARNING) << "MSM messages for observables from different systems are not defined";  // print two messages?
        }
    if (msg_number == 0)
        {
            LOG(WARNING) << "Invalid ephemeris provided";
            msg_number = 1075;
        }

    const std::string header = Rtcm::get_MSM_header(msg_number,
        obs_time,
        observables,
        ref_id,
        clock_steering_indicator,
        external_clock_indicator,
        smooth_int,
        divergence_free,
        more_messages);

    const std::string sat_data = Rtcm::get_MSM_5_content_sat_data(observables);

    const std::string signal_data = Rtcm::get_MSM_5_content_signal_data(gps_eph, gps_cnav_eph, gal_eph, glo_gnav_eph, obs_time, observables);

    std::string message = build_message(header + sat_data + signal_data);
    if (server_is_running)
        {
            rtcm_message_queue->push(message);
        }

    return message;
}


std::string Rtcm::get_MSM_5_content_sat_data(const std::map<int32_t, Gnss_Synchro>& observables)
{
    std::string sat_data;
    std::string first_data_type;
    std::string second_data_type;
    std::string third_data_type;
    std::string fourth_data_type;

    Rtcm::set_DF394(observables);
    const uint32_t num_satellites = DF394.count();
    const uint32_t numobs = observables.size();

    auto observables_vector = std::vector<std::pair<int32_t, Gnss_Synchro>>();
    observables_vector.reserve(numobs);
    std::map<int32_t, Gnss_Synchro>::const_iterator gnss_synchro_iter;
    auto pos = std::vector<uint32_t>();
    pos.reserve(numobs);
    std::vector<uint32_t>::iterator it;

    for (gnss_synchro_iter = observables.cbegin();
        gnss_synchro_iter != observables.cend();
        gnss_synchro_iter++)
        {
            it = std::find(pos.begin(), pos.end(), 65 - gnss_synchro_iter->second.PRN);
            if (it == pos.end())
                {
                    pos.push_back(65 - gnss_synchro_iter->second.PRN);
                    observables_vector.emplace_back(*gnss_synchro_iter);
                }
        }

    const std::vector<std::pair<int32_t, Gnss_Synchro>> ordered_by_PRN_pos = Rtcm::sort_by_PRN_mask(observables_vector);

    for (uint32_t nsat = 0; nsat < num_satellites; nsat++)
        {
            Rtcm::set_DF397(ordered_by_PRN_pos.at(nsat).second);
            Rtcm::set_DF398(ordered_by_PRN_pos.at(nsat).second);
            Rtcm::set_DF399(ordered_by_PRN_pos.at(nsat).second);
            auto reserved = std::bitset<4>("0000");
            first_data_type += DF397.to_string();
            second_data_type += reserved.to_string();
            third_data_type += DF398.to_string();
            fourth_data_type += DF399.to_string();
        }
    sat_data = first_data_type + second_data_type + third_data_type + fourth_data_type;
    return sat_data;
}


std::string Rtcm::get_MSM_5_content_signal_data(const Gps_Ephemeris& ephNAV,
    const Gps_CNAV_Ephemeris& ephCNAV,
    const Galileo_Ephemeris& ephFNAV,
    const Glonass_Gnav_Ephemeris& ephGNAV,
    double obs_time,
    const std::map<int32_t, Gnss_Synchro>& observables)
{
    std::string signal_data;
    std::string first_data_type;
    std::string second_data_type;
    std::string third_data_type;
    std::string fourth_data_type;
    std::string fifth_data_type;
    std::string sixth_data_type;

    const uint32_t Ncells = observables.size();

    auto observables_vector = std::vector<std::pair<int32_t, Gnss_Synchro>>();
    observables_vector.reserve(Ncells);
    std::map<int32_t, Gnss_Synchro>::const_iterator map_iter;

    for (map_iter = observables.cbegin();
        map_iter != observables.cend();
        map_iter++)
        {
            observables_vector.emplace_back(*map_iter);
        }

    std::vector<std::pair<int32_t, Gnss_Synchro>> ordered_by_signal = Rtcm::sort_by_signal(observables_vector);
    std::reverse(ordered_by_signal.begin(), ordered_by_signal.end());
    const std::vector<std::pair<int32_t, Gnss_Synchro>> ordered_by_PRN_pos = Rtcm::sort_by_PRN_mask(ordered_by_signal);

    for (uint32_t cell = 0; cell < Ncells; cell++)
        {
            Rtcm::set_DF400(ordered_by_PRN_pos.at(cell).second);
            Rtcm::set_DF401(ordered_by_PRN_pos.at(cell).second);
            Rtcm::set_DF402(ephNAV, ephCNAV, ephFNAV, ephGNAV, obs_time, ordered_by_PRN_pos.at(cell).second);
            Rtcm::set_DF420(ordered_by_PRN_pos.at(cell).second);
            Rtcm::set_DF403(ordered_by_PRN_pos.at(cell).second);
            Rtcm::set_DF404(ordered_by_PRN_pos.at(cell).second);
            first_data_type += DF400.to_string();
            second_data_type += DF401.to_string();
            third_data_type += DF402.to_string();
            fourth_data_type += DF420.to_string();
            fifth_data_type += DF403.to_string();
            sixth_data_type += DF404.to_string();
        }

    signal_data = first_data_type + second_data_type + third_data_type + fourth_data_type + fifth_data_type + sixth_data_type;
    return signal_data;
}


// **********************************************************************************************
//
//   MESSAGE TYPE MSM6 (FULL PSEUDORANGES AND PHASERANGES PLUS CNR, HIGH RESOLUTION)
//
// **********************************************************************************************

std::string Rtcm::print_MSM_6(const Gps_Ephemeris& gps_eph,
    const Gps_CNAV_Ephemeris& gps_cnav_eph,
    const Galileo_Ephemeris& gal_eph,
    const Glonass_Gnav_Ephemeris& glo_gnav_eph,
    double obs_time,
    const std::map<int32_t, Gnss_Synchro>& observables,
    uint32_t ref_id,
    uint32_t clock_steering_indicator,
    uint32_t external_clock_indicator,
    int32_t smooth_int,
    bool divergence_free,
    bool more_messages)
{
    uint32_t msg_number = 0;
    if (gps_eph.PRN != 0)
        {
            msg_number = 1076;
        }
    if (gps_cnav_eph.PRN != 0)
        {
            msg_number = 1076;
        }
    if (glo_gnav_eph.PRN != 0)
        {
            msg_number = 1086;
        }
    if (gal_eph.PRN != 0)
        {
            msg_number = 1096;
        }
    if (((gps_eph.PRN != 0) || (gps_cnav_eph.PRN != 0)) && (gal_eph.PRN != 0) && (glo_gnav_eph.PRN != 0))
        {
            LOG(WARNING) << "MSM messages for observables from different systems are not defined";  // print two messages?
        }
    if (msg_number == 0)
        {
            LOG(WARNING) << "Invalid ephemeris provided";
            msg_number = 1076;
        }

    const std::string header = Rtcm::get_MSM_header(msg_number,
        obs_time,
        observables,
        ref_id,
        clock_steering_indicator,
        external_clock_indicator,
        smooth_int,
        divergence_free,
        more_messages);

    const std::string sat_data = Rtcm::get_MSM_4_content_sat_data(observables);

    const std::string signal_data = Rtcm::get_MSM_6_content_signal_data(gps_eph, gps_cnav_eph, gal_eph, glo_gnav_eph, obs_time, observables);

    std::string message = build_message(header + sat_data + signal_data);
    if (server_is_running)
        {
            rtcm_message_queue->push(message);
        }

    return message;
}


std::string Rtcm::get_MSM_6_content_signal_data(const Gps_Ephemeris& ephNAV,
    const Gps_CNAV_Ephemeris& ephCNAV,
    const Galileo_Ephemeris& ephFNAV,
    const Glonass_Gnav_Ephemeris& ephGNAV,
    double obs_time,
    const std::map<int32_t, Gnss_Synchro>& observables)
{
    std::string signal_data;
    std::string first_data_type;
    std::string second_data_type;
    std::string third_data_type;
    std::string fourth_data_type;
    std::string fifth_data_type;

    const uint32_t Ncells = observables.size();

    auto observables_vector = std::vector<std::pair<int32_t, Gnss_Synchro>>();
    observables_vector.reserve(Ncells);
    std::map<int32_t, Gnss_Synchro>::const_iterator map_iter;

    for (map_iter = observables.cbegin();
        map_iter != observables.cend();
        map_iter++)
        {
            observables_vector.emplace_back(*map_iter);
        }

    std::vector<std::pair<int32_t, Gnss_Synchro>> ordered_by_signal = Rtcm::sort_by_signal(observables_vector);
    std::reverse(ordered_by_signal.begin(), ordered_by_signal.end());
    const std::vector<std::pair<int32_t, Gnss_Synchro>> ordered_by_PRN_pos = Rtcm::sort_by_PRN_mask(ordered_by_signal);

    for (uint32_t cell = 0; cell < Ncells; cell++)
        {
            Rtcm::set_DF405(ordered_by_PRN_pos.at(cell).second);
            Rtcm::set_DF406(ordered_by_PRN_pos.at(cell).second);
            Rtcm::set_DF407(ephNAV, ephCNAV, ephFNAV, ephGNAV, obs_time, ordered_by_PRN_pos.at(cell).second);
            Rtcm::set_DF420(ordered_by_PRN_pos.at(cell).second);
            Rtcm::set_DF408(ordered_by_PRN_pos.at(cell).second);
            first_data_type += DF405.to_string();
            second_data_type += DF406.to_string();
            third_data_type += DF407.to_string();
            fourth_data_type += DF420.to_string();
            fifth_data_type += DF408.to_string();
        }

    signal_data = first_data_type + second_data_type + third_data_type + fourth_data_type + fifth_data_type;
    return signal_data;
}


// **********************************************************************************************
//
//   MESSAGE TYPE MSM7 (FULL PSEUDORANGES, PHASERANGES, PHASERANGERATE AND CNR, HIGH RESOLUTION)
//
// **********************************************************************************************

std::string Rtcm::print_MSM_7(const Gps_Ephemeris& gps_eph,
    const Gps_CNAV_Ephemeris& gps_cnav_eph,
    const Galileo_Ephemeris& gal_eph,
    const Glonass_Gnav_Ephemeris& glo_gnav_eph,
    double obs_time,
    const std::map<int32_t, Gnss_Synchro>& observables,
    uint32_t ref_id,
    uint32_t clock_steering_indicator,
    uint32_t external_clock_indicator,
    int32_t smooth_int,
    bool divergence_free,
    bool more_messages)
{
    uint32_t msg_number = 0;
    if (gps_eph.PRN != 0)
        {
            msg_number = 1077;
        }
    if (gps_cnav_eph.PRN != 0)
        {
            msg_number = 1077;
        }
    if (glo_gnav_eph.PRN != 0)
        {
            msg_number = 1087;
        }
    if (gal_eph.PRN != 0)
        {
            msg_number = 1097;
        }
    if (((gps_eph.PRN != 0) || (gps_cnav_eph.PRN != 0)) && (glo_gnav_eph.PRN != 0) && (gal_eph.PRN != 0))
        {
            LOG(WARNING) << "MSM messages for observables from different systems are not defined";  // print two messages?
        }
    if (msg_number == 0)
        {
            LOG(WARNING) << "Invalid ephemeris provided";
            msg_number = 1076;
        }

    const std::string header = Rtcm::get_MSM_header(msg_number,
        obs_time,
        observables,
        ref_id,
        clock_steering_indicator,
        external_clock_indicator,
        smooth_int,
        divergence_free,
        more_messages);

    const std::string sat_data = Rtcm::get_MSM_5_content_sat_data(observables);

    const std::string signal_data = Rtcm::get_MSM_7_content_signal_data(gps_eph, gps_cnav_eph, gal_eph, glo_gnav_eph, obs_time, observables);

    std::string message = build_message(header + sat_data + signal_data);
    if (server_is_running)
        {
            rtcm_message_queue->push(message);
        }

    return message;
}


std::string Rtcm::get_MSM_7_content_signal_data(const Gps_Ephemeris& ephNAV,
    const Gps_CNAV_Ephemeris& ephCNAV,
    const Galileo_Ephemeris& ephFNAV,
    const Glonass_Gnav_Ephemeris& ephGNAV,
    double obs_time,
    const std::map<int32_t, Gnss_Synchro>& observables)
{
    std::string signal_data;
    std::string first_data_type;
    std::string second_data_type;
    std::string third_data_type;
    std::string fourth_data_type;
    std::string fifth_data_type;
    std::string sixth_data_type;

    const uint32_t Ncells = observables.size();

    auto observables_vector = std::vector<std::pair<int32_t, Gnss_Synchro>>();
    observables_vector.reserve(Ncells);
    std::map<int32_t, Gnss_Synchro>::const_iterator map_iter;

    for (map_iter = observables.cbegin();
        map_iter != observables.cend();
        map_iter++)
        {
            observables_vector.emplace_back(*map_iter);
        }

    std::vector<std::pair<int32_t, Gnss_Synchro>> ordered_by_signal = Rtcm::sort_by_signal(observables_vector);
    std::reverse(ordered_by_signal.begin(), ordered_by_signal.end());
    const std::vector<std::pair<int32_t, Gnss_Synchro>> ordered_by_PRN_pos = Rtcm::sort_by_PRN_mask(ordered_by_signal);

    for (uint32_t cell = 0; cell < Ncells; cell++)
        {
            Rtcm::set_DF405(ordered_by_PRN_pos.at(cell).second);
            Rtcm::set_DF406(ordered_by_PRN_pos.at(cell).second);
            Rtcm::set_DF407(ephNAV, ephCNAV, ephFNAV, ephGNAV, obs_time, ordered_by_PRN_pos.at(cell).second);
            Rtcm::set_DF420(ordered_by_PRN_pos.at(cell).second);
            Rtcm::set_DF408(ordered_by_PRN_pos.at(cell).second);
            Rtcm::set_DF404(ordered_by_PRN_pos.at(cell).second);
            first_data_type += DF405.to_string();
            second_data_type += DF406.to_string();
            third_data_type += DF407.to_string();
            fourth_data_type += DF420.to_string();
            fifth_data_type += DF408.to_string();
            sixth_data_type += DF404.to_string();
        }

    signal_data = first_data_type + second_data_type + third_data_type + fourth_data_type + fifth_data_type + sixth_data_type;
    return signal_data;
}

// SSR

uint8_t Rtcm::ssr_update_interval(uint16_t validity_seconds) const
{
    uint8_t ssr_update_interval = 0;
    if (validity_seconds > 0)
        {
            if (validity_seconds < 2)
                {
                    ssr_update_interval = 0;
                }
            else if (validity_seconds < 5)
                {
                    ssr_update_interval = 1;
                }
            else if (validity_seconds < 10)
                {
                    ssr_update_interval = 2;
                }
            else if (validity_seconds < 15)
                {
                    ssr_update_interval = 3;
                }
            else if (validity_seconds < 30)
                {
                    ssr_update_interval = 4;
                }
            else if (validity_seconds < 60)
                {
                    ssr_update_interval = 5;
                }
            else if (validity_seconds < 120)
                {
                    ssr_update_interval = 6;
                }
            else if (validity_seconds < 240)
                {
                    ssr_update_interval = 7;
                }
            else if (validity_seconds < 300)
                {
                    ssr_update_interval = 8;
                }
            else if (validity_seconds < 600)
                {
                    ssr_update_interval = 9;
                }
            else if (validity_seconds < 900)
                {
                    ssr_update_interval = 10;
                }
            else if (validity_seconds < 1800)
                {
                    ssr_update_interval = 11;
                }
            else if (validity_seconds < 3600)
                {
                    ssr_update_interval = 12;
                }
            else if (validity_seconds < 7200)
                {
                    ssr_update_interval = 13;
                }
            else if (validity_seconds < 10800)
                {
                    ssr_update_interval = 14;
                }
            else
                {
                    ssr_update_interval = 15;
                }
        }
    return ssr_update_interval;
}


std::vector<std::string> Rtcm::print_IGM01(const Galileo_HAS_data& has_data)
{
    std::vector<std::string> msgs;
    const uint8_t nsys = has_data.Nsys;
    bool ssr_multiple_msg_indicator = true;
    for (uint8_t sys = 0; sys < nsys; sys++)
        {
            if (sys == nsys - 1)
                {
                    ssr_multiple_msg_indicator = false;  // last message of a sequence
                }
            const std::string header = Rtcm::get_IGM01_header(has_data, sys, ssr_multiple_msg_indicator);
            const std::string sat_data = Rtcm::get_IGM01_content_sat(has_data, sys);
            std::string message = build_message(header + sat_data);
            if (server_is_running)
                {
                    rtcm_message_queue->push(message);
                }
            msgs.push_back(std::move(message));
        }
    return msgs;
}


std::vector<std::string> Rtcm::print_IGM02(const Galileo_HAS_data& has_data)
{
    std::vector<std::string> msgs;
    const uint8_t nsys = has_data.Nsys;
    bool ssr_multiple_msg_indicator = true;
    for (uint8_t sys = 0; sys < nsys; sys++)
        {
            if (sys == nsys - 1)
                {
                    ssr_multiple_msg_indicator = false;  // last message of a sequence
                }
            const std::string header = Rtcm::get_IGM02_header(has_data, sys, ssr_multiple_msg_indicator);
            const std::string sat_data = Rtcm::get_IGM02_content_sat(has_data, sys);
            std::string message = build_message(header + sat_data);
            if (server_is_running)
                {
                    rtcm_message_queue->push(message);
                }
            msgs.push_back(std::move(message));
        }
    return msgs;
}


std::vector<std::string> Rtcm::print_IGM03(const Galileo_HAS_data& has_data)
{
    std::vector<std::string> msgs;
    const uint8_t nsys = has_data.Nsys;
    bool ssr_multiple_msg_indicator = true;
    for (uint8_t sys = 0; sys < nsys; sys++)
        {
            if (sys == nsys - 1)
                {
                    ssr_multiple_msg_indicator = false;  // last message of a sequence
                }
            const std::string header = Rtcm::get_IGM03_header(has_data, sys, ssr_multiple_msg_indicator);
            const std::string sat_data = Rtcm::get_IGM03_content_sat(has_data, sys);
            std::string message = build_message(header + sat_data);
            if (server_is_running)
                {
                    rtcm_message_queue->push(message);
                }
            msgs.push_back(std::move(message));
        }
    return msgs;
}


std::vector<std::string> Rtcm::print_IGM05(const Galileo_HAS_data& has_data)
{
    std::vector<std::string> msgs;
    const uint8_t nsys = has_data.Nsys;
    bool ssr_multiple_msg_indicator = true;
    for (uint8_t sys = 0; sys < nsys; sys++)
        {
            if (sys == nsys - 1)
                {
                    ssr_multiple_msg_indicator = false;  // last message of a sequence
                }
            const std::string header = Rtcm::get_IGM05_header(has_data, sys, ssr_multiple_msg_indicator);
            const std::string sat_data = Rtcm::get_IGM05_content_sat(has_data, sys);
            if (!sat_data.empty())
                {
                    std::string message = build_message(header + sat_data);
                    if (server_is_running)
                        {
                            rtcm_message_queue->push(message);
                        }
                    msgs.push_back(std::move(message));
                }
        }
    return msgs;
}


std::string Rtcm::get_IGM01_header(const Galileo_HAS_data& has_data, uint8_t nsys, bool ssr_multiple_msg_indicator)
{
    std::string header;

    uint32_t tow = has_data.tow;
    uint16_t ssr_provider_id = 0;                    // ?
    uint8_t igm_version = 0;                         // ?
    uint8_t ssr_solution_id = 0;                     // ?
    auto iod_ssr = has_data.header.iod_set_id % 15;  // ?? HAS IOD is 0-31
    bool regional_indicator = false;                 // ?

    uint8_t subtype_msg_number = 0;
    if (has_data.gnss_id_mask[nsys] == 0)  // GPS
        {
            subtype_msg_number = 21;
        }
    else if (has_data.gnss_id_mask[nsys] == 2)  // Galileo
        {
            subtype_msg_number = 61;
        }

    uint8_t validity_index = has_data.validity_interval_index_orbit_corrections;
    uint16_t validity_seconds = has_data.get_validity_interval_s(validity_index);
    uint8_t ssr_update_interval_ = ssr_update_interval(validity_seconds);
    uint8_t Nsat = has_data.get_num_satellites()[nsys];

    Rtcm::set_DF002(4076);  // Always “4076” for IGS Proprietary Messages
    Rtcm::set_IDF001(igm_version);
    Rtcm::set_IDF002(subtype_msg_number);
    Rtcm::set_IDF003(tow);
    Rtcm::set_IDF004(ssr_update_interval_);
    Rtcm::set_IDF005(ssr_multiple_msg_indicator);
    Rtcm::set_IDF007(iod_ssr);
    Rtcm::set_IDF008(ssr_provider_id);
    Rtcm::set_IDF009(ssr_solution_id);
    Rtcm::set_IDF006(regional_indicator);
    Rtcm::set_IDF010(Nsat);

    header += DF002.to_string() + IDF001.to_string() + IDF002.to_string() +
              IDF003.to_string() + IDF004.to_string() + IDF005.to_string() +
              IDF007.to_string() + IDF008.to_string() + IDF009.to_string() +
              IDF006.to_string() + IDF010.to_string();
    return header;
}


std::string Rtcm::get_IGM01_content_sat(const Galileo_HAS_data& has_data, uint8_t nsys_index)
{
    std::string content;

    std::vector<int> prn = has_data.get_PRNs_in_mask(nsys_index);
    std::vector<uint16_t> gnss_iod = has_data.get_gnss_iod(nsys_index);
    std::vector<float> delta_orbit_radial_m = has_data.get_delta_radial_m(nsys_index);
    std::vector<float> delta_orbit_in_track_m = has_data.get_delta_in_track_m(nsys_index);
    std::vector<float> delta_orbit_cross_track_m = has_data.get_delta_cross_track_m(nsys_index);

    const uint8_t num_sats_in_this_system = has_data.get_num_satellites()[nsys_index];
    for (uint8_t sat = 0; sat < num_sats_in_this_system; sat++)
        {
            Rtcm::set_IDF011(static_cast<uint8_t>(prn[sat]));
            Rtcm::set_IDF012(static_cast<uint8_t>(gnss_iod[sat] % 255));  // 8 LSBs
            Rtcm::set_IDF013(delta_orbit_radial_m[sat]);
            Rtcm::set_IDF014(delta_orbit_in_track_m[sat]);
            Rtcm::set_IDF016(0.0);  //  dot_orbit_delta_track_m_s
            Rtcm::set_IDF015(delta_orbit_cross_track_m[sat]);
            Rtcm::set_IDF017(0.0);  // dot_orbit_delta_in_track_m_s
            Rtcm::set_IDF018(0.0);  // dot_orbit_delta_cross_track_m_s

            content += IDF011.to_string() + IDF012.to_string() + IDF013.to_string() +
                       IDF014.to_string() + IDF016.to_string() + IDF015.to_string() +
                       IDF017.to_string() + IDF018.to_string();
        }

    return content;
}


std::string Rtcm::get_IGM02_header(const Galileo_HAS_data& has_data, uint8_t nsys, bool ssr_multiple_msg_indicator)
{
    std::string header;

    uint32_t tow = has_data.tow;
    uint16_t ssr_provider_id = 0;                    // ?
    uint8_t igm_version = 0;                         // ?
    uint8_t ssr_solution_id = 0;                     // ?
    auto iod_ssr = has_data.header.iod_set_id % 15;  // ?? HAS IOD is 0-31

    uint8_t subtype_msg_number = 0;
    if (has_data.gnss_id_mask[nsys] == 0)  // GPS
        {
            subtype_msg_number = 22;
        }
    else if (has_data.gnss_id_mask[nsys] == 2)  // Galileo
        {
            subtype_msg_number = 62;
        }

    uint8_t validity_index = has_data.validity_interval_index_orbit_corrections;
    uint16_t validity_seconds = has_data.get_validity_interval_s(validity_index);
    uint8_t ssr_update_interval_ = ssr_update_interval(validity_seconds);
    uint8_t Nsat = has_data.get_num_satellites()[nsys];

    Rtcm::set_DF002(4076);  // Always “4076” for IGS Proprietary Messages
    Rtcm::set_IDF001(igm_version);
    Rtcm::set_IDF002(subtype_msg_number);
    Rtcm::set_IDF003(tow);
    Rtcm::set_IDF004(ssr_update_interval_);
    Rtcm::set_IDF005(ssr_multiple_msg_indicator);
    Rtcm::set_IDF007(iod_ssr);
    Rtcm::set_IDF008(ssr_provider_id);
    Rtcm::set_IDF009(ssr_solution_id);
    Rtcm::set_IDF010(Nsat);

    header += DF002.to_string() + IDF001.to_string() + IDF002.to_string() +
              IDF003.to_string() + IDF004.to_string() + IDF005.to_string() +
              IDF007.to_string() + IDF008.to_string() + IDF009.to_string() +
              IDF010.to_string();
    return header;
}


std::string Rtcm::get_IGM02_content_sat(const Galileo_HAS_data& has_data, uint8_t nsys_index)
{
    std::string content;

    const uint8_t num_sats_in_this_system = has_data.get_num_satellites()[nsys_index];

    std::vector<int> prn = has_data.get_PRNs_in_mask(nsys_index);

    std::vector<float> delta_clock_c0 = has_data.get_delta_clock_correction_m(nsys_index);
    std::vector<float> delta_clock_c1(num_sats_in_this_system);
    std::vector<float> delta_clock_c2(num_sats_in_this_system);

    for (uint8_t sat = 0; sat < num_sats_in_this_system; sat++)
        {
            Rtcm::set_IDF011(static_cast<uint8_t>(prn[sat]));
            Rtcm::set_IDF019(delta_clock_c0[sat]);
            Rtcm::set_IDF020(delta_clock_c1[sat]);
            Rtcm::set_IDF021(delta_clock_c2[sat]);

            content += IDF011.to_string() + IDF019.to_string() + IDF020.to_string() +
                       IDF021.to_string();
        }

    return content;
}


std::string Rtcm::get_IGM03_header(const Galileo_HAS_data& has_data, uint8_t nsys, bool ssr_multiple_msg_indicator)
{
    std::string header;

    uint32_t tow = has_data.tow;
    uint16_t ssr_provider_id = 0;                    // ?
    uint8_t igm_version = 0;                         // ?
    uint8_t ssr_solution_id = 0;                     // ?
    auto iod_ssr = has_data.header.iod_set_id % 15;  // ?? HAS IOD is 0-31
    bool regional_indicator = false;                 // ?

    uint8_t subtype_msg_number = 0;
    if (has_data.gnss_id_mask[nsys] == 0)  // GPS
        {
            subtype_msg_number = 23;
        }
    else if (has_data.gnss_id_mask[nsys] == 2)  // Galileo
        {
            subtype_msg_number = 63;
        }

    uint8_t validity_index = has_data.validity_interval_index_orbit_corrections;
    uint16_t validity_seconds = has_data.get_validity_interval_s(validity_index);
    uint8_t ssr_update_interval_ = ssr_update_interval(validity_seconds);
    uint8_t Nsat = has_data.get_num_satellites()[nsys];

    Rtcm::set_DF002(4076);  // Always “4076” for IGS Proprietary Messages
    Rtcm::set_IDF001(igm_version);
    Rtcm::set_IDF002(subtype_msg_number);
    Rtcm::set_IDF003(tow);
    Rtcm::set_IDF004(ssr_update_interval_);
    Rtcm::set_IDF005(ssr_multiple_msg_indicator);
    Rtcm::set_IDF007(iod_ssr);
    Rtcm::set_IDF008(ssr_provider_id);
    Rtcm::set_IDF009(ssr_solution_id);
    Rtcm::set_IDF006(regional_indicator);
    Rtcm::set_IDF010(Nsat);

    header += DF002.to_string() + IDF001.to_string() + IDF002.to_string() +
              IDF003.to_string() + IDF004.to_string() + IDF005.to_string() +
              IDF007.to_string() + IDF008.to_string() + IDF009.to_string() +
              IDF006.to_string() + IDF010.to_string();
    return header;
}


std::string Rtcm::get_IGM03_content_sat(const Galileo_HAS_data& has_data, uint8_t nsys_index)
{
    std::string content;

    const uint8_t num_sats_in_this_system = has_data.get_num_satellites()[nsys_index];

    std::vector<int> prn = has_data.get_PRNs_in_mask(nsys_index);
    std::vector<uint16_t> gnss_iod = has_data.get_gnss_iod(nsys_index);
    std::vector<float> delta_orbit_radial_m = has_data.get_delta_radial_m(nsys_index);
    std::vector<float> delta_orbit_in_track_m = has_data.get_delta_in_track_m(nsys_index);
    std::vector<float> delta_orbit_cross_track_m = has_data.get_delta_cross_track_m(nsys_index);
    std::vector<float> delta_clock_c0 = has_data.get_delta_clock_correction_m(nsys_index);
    std::vector<float> delta_clock_c1(num_sats_in_this_system);
    std::vector<float> delta_clock_c2(num_sats_in_this_system);

    for (uint8_t sat = 0; sat < num_sats_in_this_system; sat++)
        {
            Rtcm::set_IDF011(static_cast<uint8_t>(prn[sat]));
            Rtcm::set_IDF012(static_cast<uint8_t>(gnss_iod[sat] % 255));  // 8 LSBs
            Rtcm::set_IDF013(delta_orbit_radial_m[sat]);
            Rtcm::set_IDF014(delta_orbit_in_track_m[sat]);
            Rtcm::set_IDF015(delta_orbit_cross_track_m[sat]);
            Rtcm::set_IDF016(0.0);  // dot_orbit_delta_track_m_s
            Rtcm::set_IDF017(0.0);  // dot_orbit_delta_in_track_m_s
            Rtcm::set_IDF018(0.0);  // dot_orbit_delta_cross_track_m_s
            Rtcm::set_IDF019(delta_clock_c0[sat]);
            Rtcm::set_IDF020(delta_clock_c1[sat]);
            Rtcm::set_IDF021(delta_clock_c2[sat]);

            content += IDF011.to_string() + IDF012.to_string() + IDF013.to_string() +
                       IDF014.to_string() + IDF015.to_string() + IDF016.to_string() +
                       IDF017.to_string() + IDF018.to_string() + DF019.to_string() +
                       IDF020.to_string() + IDF021.to_string();
        }

    return content;
}


std::string Rtcm::get_IGM05_header(const Galileo_HAS_data& has_data, uint8_t nsys, bool ssr_multiple_msg_indicator)
{
    std::string header;

    uint32_t tow = has_data.tow;
    uint16_t ssr_provider_id = 0;                    // ?
    uint8_t igm_version = 0;                         // ?
    uint8_t ssr_solution_id = 0;                     // ?
    auto iod_ssr = has_data.header.iod_set_id % 15;  // ?? HAS IOD is 0-31

    uint8_t subtype_msg_number = 0;
    if (has_data.gnss_id_mask[nsys] == 0)  // GPS
        {
            subtype_msg_number = 25;
        }
    else if (has_data.gnss_id_mask[nsys] == 2)  // Galileo
        {
            subtype_msg_number = 65;
        }

    uint8_t validity_index = has_data.validity_interval_index_orbit_corrections;
    uint16_t validity_seconds = has_data.get_validity_interval_s(validity_index);
    uint8_t ssr_update_interval_ = ssr_update_interval(validity_seconds);
    uint8_t Nsat = has_data.get_num_satellites()[nsys];

    Rtcm::set_DF002(4076);  // Always “4076” for IGS Proprietary Messages
    Rtcm::set_IDF001(igm_version);
    Rtcm::set_IDF002(subtype_msg_number);
    Rtcm::set_IDF003(tow);
    Rtcm::set_IDF004(ssr_update_interval_);
    Rtcm::set_IDF005(ssr_multiple_msg_indicator);
    Rtcm::set_IDF007(iod_ssr);
    Rtcm::set_IDF008(ssr_provider_id);
    Rtcm::set_IDF009(ssr_solution_id);
    Rtcm::set_IDF010(Nsat);

    header += DF002.to_string() + IDF001.to_string() + IDF002.to_string() +
              IDF003.to_string() + IDF004.to_string() + IDF005.to_string() +
              IDF007.to_string() + IDF008.to_string() + IDF009.to_string() +
              IDF010.to_string();
    return header;
}


std::string Rtcm::get_IGM05_content_sat(const Galileo_HAS_data& has_data, uint8_t nsys_index)
{
    std::string content;

    const uint8_t num_sats_in_this_system = has_data.get_num_satellites()[nsys_index];
    std::vector<int> prn = has_data.get_PRNs_in_mask(nsys_index);
    std::vector<std::vector<float>> code_bias_m = has_data.get_code_bias_m();

    for (uint8_t sat = 0; sat < num_sats_in_this_system; sat++)
        {
            uint8_t num_bias_processed = has_data.get_signals_in_mask(nsys_index).size();

            uint8_t valid_num_bias_processed = 0;
            std::vector<uint8_t> gnss_signal_tracking_mode_id_v;
            std::vector<bool> valid_bias_v;

            for (uint8_t code = 0; code < num_bias_processed; code++)
                {
                    std::string code_string = has_data.get_signals_in_mask(nsys_index)[code];
                    if (has_data.gnss_id_mask[nsys_index] == 0)  // GPS
                        {
                            if (code_string == "L1 C/A")
                                {
                                    gnss_signal_tracking_mode_id_v.push_back(0);
                                    valid_bias_v.push_back(true);
                                    valid_num_bias_processed++;
                                }
                            else if (code_string == "L1C(D)")
                                {
                                    gnss_signal_tracking_mode_id_v.push_back(3);
                                    valid_bias_v.push_back(true);
                                    valid_num_bias_processed++;
                                }
                            else if (code_string == "L1C(P)")
                                {
                                    gnss_signal_tracking_mode_id_v.push_back(4);
                                    valid_bias_v.push_back(true);
                                    valid_num_bias_processed++;
                                }
                            else if (code_string == "L2 CM")
                                {
                                    gnss_signal_tracking_mode_id_v.push_back(7);
                                    valid_bias_v.push_back(true);
                                    valid_num_bias_processed++;
                                }
                            else if (code_string == "L2 CL")
                                {
                                    gnss_signal_tracking_mode_id_v.push_back(8);
                                    valid_bias_v.push_back(true);
                                    valid_num_bias_processed++;
                                }
                            else if (code_string == "L5 I")
                                {
                                    gnss_signal_tracking_mode_id_v.push_back(14);
                                    valid_bias_v.push_back(true);
                                    valid_num_bias_processed++;
                                }
                            else if (code_string == "L5 Q")
                                {
                                    gnss_signal_tracking_mode_id_v.push_back(15);
                                    valid_bias_v.push_back(true);
                                    valid_num_bias_processed++;
                                }
                            else
                                {
                                    gnss_signal_tracking_mode_id_v.push_back(0);
                                    valid_bias_v.push_back(false);
                                }
                        }
                    else if (has_data.gnss_id_mask[nsys_index] == 2)  // Galileo
                        {
                            if (code_string == "E1-B I/NAV OS")
                                {
                                    gnss_signal_tracking_mode_id_v.push_back(1);
                                    valid_bias_v.push_back(true);
                                    valid_num_bias_processed++;
                                }
                            else if (code_string == "E1-C")
                                {
                                    gnss_signal_tracking_mode_id_v.push_back(2);
                                    valid_bias_v.push_back(true);
                                    valid_num_bias_processed++;
                                }
                            else if (code_string == "E5a-I F/NAV OS")
                                {
                                    gnss_signal_tracking_mode_id_v.push_back(5);
                                    valid_bias_v.push_back(true);
                                    valid_num_bias_processed++;
                                }
                            else if (code_string == "E5a-Q")
                                {
                                    gnss_signal_tracking_mode_id_v.push_back(6);
                                    valid_bias_v.push_back(true);
                                    valid_num_bias_processed++;
                                }
                            else if (code_string == "E5b-I I/NAV OS")
                                {
                                    gnss_signal_tracking_mode_id_v.push_back(8);
                                    valid_bias_v.push_back(true);
                                    valid_num_bias_processed++;
                                }
                            else if (code_string == "E5b-Q")
                                {
                                    gnss_signal_tracking_mode_id_v.push_back(9);
                                    valid_bias_v.push_back(true);
                                    valid_num_bias_processed++;
                                }
                            else if (code_string == "E6-B C/NAV HAS")
                                {
                                    gnss_signal_tracking_mode_id_v.push_back(15);
                                    valid_bias_v.push_back(true);
                                    valid_num_bias_processed++;
                                }
                            else if (code_string == "E6-C")
                                {
                                    gnss_signal_tracking_mode_id_v.push_back(16);
                                    valid_bias_v.push_back(true);
                                    valid_num_bias_processed++;
                                }
                            else
                                {
                                    gnss_signal_tracking_mode_id_v.push_back(0);
                                    valid_bias_v.push_back(false);
                                }
                        }
                    else
                        {
                            gnss_signal_tracking_mode_id_v.push_back(0);
                            valid_bias_v.push_back(false);
                        }
                }

            if (valid_num_bias_processed > 0)
                {
                    Rtcm::set_IDF011(static_cast<uint8_t>(prn[sat]));
                    Rtcm::set_IDF023(valid_num_bias_processed);

                    content += IDF011.to_string() + IDF023.to_string();

                    uint8_t num_sats_in_previous_systems = 0;
                    for (uint8_t nsys = 0; nsys < nsys_index; nsys++)
                        {
                            num_sats_in_previous_systems += has_data.get_num_satellites()[nsys];
                        }
                    uint8_t sat_index = sat + num_sats_in_previous_systems;

                    for (uint8_t code = 0; code < num_bias_processed; code++)
                        {
                            if (valid_bias_v[code] == true)
                                {
                                    Rtcm::set_IDF024(gnss_signal_tracking_mode_id_v[code]);
                                    Rtcm::set_IDF025(code_bias_m[sat_index][code]);
                                    content += DF024.to_string() + IDF025.to_string();
                                }
                        }
                }
        }

    return content;
}


// *****************************************************************************************************
// Some utilities
// *****************************************************************************************************

std::vector<std::pair<int32_t, Gnss_Synchro>> Rtcm::sort_by_PRN_mask(const std::vector<std::pair<int32_t, Gnss_Synchro>>& synchro_map) const
{
    std::vector<std::pair<int32_t, Gnss_Synchro>>::const_iterator synchro_map_iter;
    std::vector<std::pair<int32_t, Gnss_Synchro>> my_vec;
    struct
    {
        bool operator()(const std::pair<int32_t, Gnss_Synchro>& a, const std::pair<int32_t, Gnss_Synchro>& b)
        {
            uint32_t value_a = 64 - a.second.PRN;
            uint32_t value_b = 64 - b.second.PRN;
            return value_a < value_b;
        }
    } has_lower_pos;

    for (synchro_map_iter = synchro_map.cbegin();
        synchro_map_iter != synchro_map.cend();
        synchro_map_iter++)

        {
            std::pair<int32_t, Gnss_Synchro> p(synchro_map_iter->first, synchro_map_iter->second);
            my_vec.push_back(p);
        }

    std::sort(my_vec.begin(), my_vec.end(), has_lower_pos);
    std::reverse(my_vec.begin(), my_vec.end());
    return my_vec;
}


std::vector<std::pair<int32_t, Gnss_Synchro>> Rtcm::sort_by_signal(const std::vector<std::pair<int32_t, Gnss_Synchro>>& synchro_map) const
{
    std::vector<std::pair<int32_t, Gnss_Synchro>>::const_iterator synchro_map_iter;
    std::vector<std::pair<int32_t, Gnss_Synchro>> my_vec;

    struct
    {
        bool operator()(const std::pair<int32_t, Gnss_Synchro>& a, const std::pair<int32_t, Gnss_Synchro>& b)
        {
            uint32_t value_a = 0;
            uint32_t value_b = 0;
            const std::string system_a(&a.second.System, 1);
            const std::string system_b(&b.second.System, 1);
            const std::string sig_a_(a.second.Signal);
            const std::string sig_a = sig_a_.substr(0, 2);
            const std::string sig_b_(b.second.Signal);
            const std::string sig_b = sig_b_.substr(0, 2);

            if (system_a == "G")
                {
                    value_a = gps_signal_map.at(sig_a);
                }

            if (system_a == "E")
                {
                    value_a = galileo_signal_map.at(sig_a);
                }

            if (system_b == "G")
                {
                    value_b = gps_signal_map.at(sig_b);
                }

            if (system_b == "E")
                {
                    value_b = galileo_signal_map.at(sig_b);
                }

            return value_a < value_b;
        }
    } has_lower_signalID;


    for (synchro_map_iter = synchro_map.cbegin();
        synchro_map_iter != synchro_map.cend();
        synchro_map_iter++)

        {
            std::pair<int32_t, Gnss_Synchro> p(synchro_map_iter->first, synchro_map_iter->second);
            my_vec.push_back(p);
        }

    std::sort(my_vec.begin(), my_vec.end(), has_lower_signalID);
    return my_vec;
}


std::map<std::string, int> Rtcm::gps_signal_map = [] {
    std::map<std::string, int> gps_signal_map_;
    // Table 3.5-91
    gps_signal_map_["1C"] = 2;
    gps_signal_map_["1P"] = 3;
    gps_signal_map_["1W"] = 4;
    gps_signal_map_["2C"] = 8;
    gps_signal_map_["2P"] = 9;
    gps_signal_map_["2W"] = 10;
    gps_signal_map_["2S"] = 15;
    gps_signal_map_["2L"] = 16;
    gps_signal_map_["2X"] = 17;
    gps_signal_map_["5I"] = 22;
    gps_signal_map_["5Q"] = 23;
    gps_signal_map_["5X"] = 24;
    gps_signal_map_["L5"] = 24;  // Workaround. TODO: check if it was I or Q
    return gps_signal_map_;
}();


std::map<std::string, int> Rtcm::galileo_signal_map = [] {
    std::map<std::string, int> galileo_signal_map_;
    // Table 3.5-100
    galileo_signal_map_["1C"] = 2;
    galileo_signal_map_["1A"] = 3;
    galileo_signal_map_["1B"] = 4;
    galileo_signal_map_["1X"] = 5;
    galileo_signal_map_["1Z"] = 6;
    galileo_signal_map_["6C"] = 8;
    galileo_signal_map_["6A"] = 9;
    galileo_signal_map_["6B"] = 10;
    galileo_signal_map_["6X"] = 11;
    galileo_signal_map_["6Z"] = 12;
    galileo_signal_map_["7I"] = 14;
    galileo_signal_map_["7Q"] = 15;
    galileo_signal_map_["7X"] = 16;
    galileo_signal_map_["8I"] = 18;
    galileo_signal_map_["8Q"] = 19;
    galileo_signal_map_["8X"] = 20;
    galileo_signal_map_["5I"] = 22;
    galileo_signal_map_["5Q"] = 23;
    galileo_signal_map_["5X"] = 24;

    galileo_signal_map_["E6"] = 10;
    return galileo_signal_map_;
}();


boost::posix_time::ptime Rtcm::compute_GPS_time(const Gps_Ephemeris& eph, double obs_time) const
{
    const double gps_t = obs_time;
    const boost::posix_time::time_duration t_duration = boost::posix_time::milliseconds(static_cast<long>((gps_t + 604800 * static_cast<double>(eph.WN)) * 1000));  // NOLINT(google-runtime-int)

    if (eph.WN < 512)
        {
            boost::posix_time::ptime p_time(boost::gregorian::date(2019, 4, 7), t_duration);
            return p_time;
        }

    boost::posix_time::ptime p_time(boost::gregorian::date(1999, 8, 22), t_duration);
    return p_time;
}


boost::posix_time::ptime Rtcm::compute_GPS_time(const Gps_CNAV_Ephemeris& eph, double obs_time) const
{
    const double gps_t = obs_time;
    const boost::posix_time::time_duration t_duration = boost::posix_time::milliseconds(static_cast<long>((gps_t + 604800 * static_cast<double>(eph.WN)) * 1000));  // NOLINT(google-runtime-int)
    boost::posix_time::ptime p_time(boost::gregorian::date(1999, 8, 22), t_duration);
    return p_time;
}


boost::posix_time::ptime Rtcm::compute_Galileo_time(const Galileo_Ephemeris& eph, double obs_time) const
{
    const double galileo_t = obs_time;
    const boost::posix_time::time_duration t_duration = boost::posix_time::milliseconds(static_cast<long>((galileo_t + 604800 * static_cast<double>(eph.WN)) * 1000));  // NOLINT(google-runtime-int)
    boost::posix_time::ptime p_time(boost::gregorian::date(1999, 8, 22), t_duration);
    return p_time;
}


boost::posix_time::ptime Rtcm::compute_GLONASS_time(const Glonass_Gnav_Ephemeris& eph, double obs_time) const
{
    boost::posix_time::ptime p_time = eph.compute_GLONASS_time(obs_time);
    return p_time;
}


uint32_t Rtcm::lock_time(const Gps_Ephemeris& eph, double obs_time, const Gnss_Synchro& gnss_synchro)
{
    boost::posix_time::ptime current_time = Rtcm::compute_GPS_time(eph, obs_time);
    boost::posix_time::ptime last_lock_time = Rtcm::gps_L1_last_lock_time[65 - gnss_synchro.PRN];
    if (last_lock_time.is_not_a_date_time())  // || CHECK LLI!!......)
        {
            Rtcm::gps_L1_last_lock_time[65 - gnss_synchro.PRN] = current_time;
        }
    boost::posix_time::time_duration lock_duration = current_time - Rtcm::gps_L1_last_lock_time[65 - gnss_synchro.PRN];
    const auto lock_time_in_seconds = static_cast<uint32_t>(lock_duration.total_seconds());
    // Debug:
    // std::cout << "lock time PRN " << gnss_synchro.PRN << ": " << lock_time_in_seconds <<  "  current time: " << current_time << '\n';
    return lock_time_in_seconds;
}


uint32_t Rtcm::lock_time(const Gps_CNAV_Ephemeris& eph, double obs_time, const Gnss_Synchro& gnss_synchro)
{
    const boost::posix_time::ptime current_time = Rtcm::compute_GPS_time(eph, obs_time);
    boost::posix_time::ptime last_lock_time = Rtcm::gps_L2_last_lock_time[65 - gnss_synchro.PRN];
    if (last_lock_time.is_not_a_date_time())  // || CHECK LLI!!......)
        {
            Rtcm::gps_L2_last_lock_time[65 - gnss_synchro.PRN] = current_time;
        }
    boost::posix_time::time_duration lock_duration = current_time - Rtcm::gps_L2_last_lock_time[65 - gnss_synchro.PRN];
    const auto lock_time_in_seconds = static_cast<uint32_t>(lock_duration.total_seconds());
    return lock_time_in_seconds;
}


uint32_t Rtcm::lock_time(const Galileo_Ephemeris& eph, double obs_time, const Gnss_Synchro& gnss_synchro)
{
    const boost::posix_time::ptime current_time = Rtcm::compute_Galileo_time(eph, obs_time);

    boost::posix_time::ptime last_lock_time;
    const std::string sig_(gnss_synchro.Signal);
    if (sig_ == "1B")
        {
            last_lock_time = Rtcm::gal_E1_last_lock_time[65 - gnss_synchro.PRN];
        }
    if ((sig_ == "5X") || (sig_ == "8X") || (sig_ == "7X"))
        {
            last_lock_time = Rtcm::gal_E5_last_lock_time[65 - gnss_synchro.PRN];
        }

    if (last_lock_time.is_not_a_date_time())  // || CHECK LLI!!......)
        {
            if (sig_ == "1B")
                {
                    Rtcm::gal_E1_last_lock_time[65 - gnss_synchro.PRN] = current_time;
                }
            if ((sig_ == "5X") || (sig_ == "8X") || (sig_ == "7X"))
                {
                    Rtcm::gal_E5_last_lock_time[65 - gnss_synchro.PRN] = current_time;
                }
        }

    boost::posix_time::time_duration lock_duration = current_time - current_time;
    if (sig_ == "1B")
        {
            lock_duration = current_time - Rtcm::gal_E1_last_lock_time[65 - gnss_synchro.PRN];
        }
    if ((sig_ == "5X") || (sig_ == "8X") || (sig_ == "7X"))
        {
            lock_duration = current_time - Rtcm::gal_E5_last_lock_time[65 - gnss_synchro.PRN];
        }

    const auto lock_time_in_seconds = static_cast<uint32_t>(lock_duration.total_seconds());
    return lock_time_in_seconds;
}


uint32_t Rtcm::lock_time(const Glonass_Gnav_Ephemeris& eph, double obs_time, const Gnss_Synchro& gnss_synchro)
{
    const boost::posix_time::ptime current_time = Rtcm::compute_GLONASS_time(eph, obs_time);

    boost::posix_time::ptime last_lock_time;
    const std::string sig_(gnss_synchro.Signal);
    if (sig_ == "1C")
        {
            last_lock_time = Rtcm::glo_L1_last_lock_time[65 - gnss_synchro.PRN];
        }
    if (sig_ == "2C")
        {
            last_lock_time = Rtcm::glo_L2_last_lock_time[65 - gnss_synchro.PRN];
        }

    if (last_lock_time.is_not_a_date_time())  // || CHECK LLI!!......)
        {
            if (sig_ == "1C")
                {
                    Rtcm::glo_L1_last_lock_time[65 - gnss_synchro.PRN] = current_time;
                }
            if (sig_ == "2C")
                {
                    Rtcm::glo_L2_last_lock_time[65 - gnss_synchro.PRN] = current_time;
                }
        }

    boost::posix_time::time_duration lock_duration = current_time - current_time;
    if (sig_ == "1C")
        {
            lock_duration = current_time - Rtcm::glo_L1_last_lock_time[65 - gnss_synchro.PRN];
        }
    if (sig_ == "2C")
        {
            lock_duration = current_time - Rtcm::glo_L2_last_lock_time[65 - gnss_synchro.PRN];
        }

    const auto lock_time_in_seconds = static_cast<uint32_t>(lock_duration.total_seconds());
    return lock_time_in_seconds;
}


uint32_t Rtcm::lock_time_indicator(uint32_t lock_time_period_s)
{
    // Table 3.4-2
    if (lock_time_period_s <= 0)
        {
            return 0;
        }
    if (lock_time_period_s < 24)
        {
            return lock_time_period_s;
        }
    if (lock_time_period_s < 72)
        {
            return (lock_time_period_s + 24) / 2;
        }
    if (lock_time_period_s < 168)
        {
            return (lock_time_period_s + 120) / 4;
        }
    if (lock_time_period_s < 360)
        {
            return (lock_time_period_s + 408) / 8;
        }
    if (lock_time_period_s < 744)
        {
            return (lock_time_period_s + 1176) / 16;
        }
    if (lock_time_period_s < 937)
        {
            return (lock_time_period_s + 3096) / 32;
        }
    return 127;
}


uint32_t Rtcm::msm_lock_time_indicator(uint32_t lock_time_period_s)
{
    // Table 3.5-74
    if (lock_time_period_s < 32)
        {
            return 0;
        }
    if (lock_time_period_s < 64)
        {
            return 1;
        }
    if (lock_time_period_s < 128)
        {
            return 2;
        }
    if (lock_time_period_s < 256)
        {
            return 3;
        }
    if (lock_time_period_s < 512)
        {
            return 4;
        }
    if (lock_time_period_s < 1024)
        {
            return 5;
        }
    if (lock_time_period_s < 2048)
        {
            return 6;
        }
    if (lock_time_period_s < 4096)
        {
            return 7;
        }
    if (lock_time_period_s < 8192)
        {
            return 8;
        }
    if (lock_time_period_s < 16384)
        {
            return 9;
        }
    if (lock_time_period_s < 32768)
        {
            return 10;
        }
    if (lock_time_period_s < 65536)
        {
            return 11;
        }
    if (lock_time_period_s < 131072)
        {
            return 12;
        }
    if (lock_time_period_s < 262144)
        {
            return 13;
        }
    if (lock_time_period_s < 524288)
        {
            return 14;
        }
    return 15;
}


// clang-format off
uint32_t Rtcm::msm_extended_lock_time_indicator(uint32_t lock_time_period_s)
{
    // Table 3.5-75
    if(                                   lock_time_period_s < 64       ) return (       lock_time_period_s                      );  // NOLINT
    if(       64 <= lock_time_period_s && lock_time_period_s < 128      ) return ( 64 + (lock_time_period_s - 64      ) / 2      );  // NOLINT
    if(      128 <= lock_time_period_s && lock_time_period_s < 256      ) return ( 96 + (lock_time_period_s - 128     ) / 4      );  // NOLINT
    if(      256 <= lock_time_period_s && lock_time_period_s < 512      ) return (128 + (lock_time_period_s - 256     ) / 8      );  // NOLINT
    if(      512 <= lock_time_period_s && lock_time_period_s < 1024     ) return (160 + (lock_time_period_s - 512     ) / 16     );  // NOLINT
    if(     1024 <= lock_time_period_s && lock_time_period_s < 2048     ) return (192 + (lock_time_period_s - 1024    ) / 32     );  // NOLINT
    if(     2048 <= lock_time_period_s && lock_time_period_s < 4096     ) return (224 + (lock_time_period_s - 2048    ) / 64     );  // NOLINT
    if(     4096 <= lock_time_period_s && lock_time_period_s < 8192     ) return (256 + (lock_time_period_s - 4096    ) / 128    );  // NOLINT
    if(     8192 <= lock_time_period_s && lock_time_period_s < 16384    ) return (288 + (lock_time_period_s - 8192    ) / 256    );  // NOLINT
    if(    16384 <= lock_time_period_s && lock_time_period_s < 32768    ) return (320 + (lock_time_period_s - 16384   ) / 512    );  // NOLINT
    if(    32768 <= lock_time_period_s && lock_time_period_s < 65536    ) return (352 + (lock_time_period_s - 32768   ) / 1024   );  // NOLINT
    if(    65536 <= lock_time_period_s && lock_time_period_s < 131072   ) return (384 + (lock_time_period_s - 65536   ) / 2048   );  // NOLINT
    if(   131072 <= lock_time_period_s && lock_time_period_s < 262144   ) return (416 + (lock_time_period_s - 131072  ) / 4096   );  // NOLINT
    if(   262144 <= lock_time_period_s && lock_time_period_s < 524288   ) return (448 + (lock_time_period_s - 262144  ) / 8192   );  // NOLINT
    if(   524288 <= lock_time_period_s && lock_time_period_s < 1048576  ) return (480 + (lock_time_period_s - 524288  ) / 16384  );  // NOLINT
    if(  1048576 <= lock_time_period_s && lock_time_period_s < 2097152  ) return (512 + (lock_time_period_s - 1048576 ) / 32768  );  // NOLINT
    if(  2097152 <= lock_time_period_s && lock_time_period_s < 4194304  ) return (544 + (lock_time_period_s - 2097152 ) / 65536  );  // NOLINT
    if(  4194304 <= lock_time_period_s && lock_time_period_s < 8388608  ) return (576 + (lock_time_period_s - 4194304 ) / 131072 );  // NOLINT
    if(  8388608 <= lock_time_period_s && lock_time_period_s < 16777216 ) return (608 + (lock_time_period_s - 8388608 ) / 262144 );  // NOLINT
    if( 16777216 <= lock_time_period_s && lock_time_period_s < 33554432 ) return (640 + (lock_time_period_s - 16777216) / 524288 );  // NOLINT
    if( 33554432 <= lock_time_period_s && lock_time_period_s < 67108864 ) return (672 + (lock_time_period_s - 33554432) / 1048576);  // NOLINT
    if( 67108864 <= lock_time_period_s                                  ) return (704                                            );  // NOLINT
    return 1023;  // will never happen
 }
// clang-format on

// *****************************************************************************************************
//
//   DATA FIELDS AS DEFINED AT RTCM STANDARD 10403.2
//
// *****************************************************************************************************

int32_t Rtcm::set_DF002(uint32_t message_number)
{
    if (message_number > 4095)
        {
            LOG(WARNING) << "RTCM message number must be between 0 and 4095, but it has been set to " << message_number;
        }
    DF002 = std::bitset<12>(message_number);
    return 0;
}


int32_t Rtcm::set_DF003(uint32_t ref_station_ID)
{
    // uint32_t station_ID = ref_station_ID;
    if (ref_station_ID > 4095)
        {
            LOG(WARNING) << "RTCM reference station ID must be between 0 and 4095, but it has been set to " << ref_station_ID;
        }
    DF003 = std::bitset<12>(ref_station_ID);
    return 0;
}


int32_t Rtcm::set_DF004(double obs_time)
{
    // TOW in milliseconds from the beginning of the GPS week, measured in GPS time
    auto tow = static_cast<uint64_t>(std::round(obs_time * 1000));
    if (tow > 604799999)
        {
            LOG(WARNING) << "To large TOW! Set to the last millisecond of the week";
            tow = 604799999;
        }
    DF004 = std::bitset<30>(tow);
    return 0;
}


int32_t Rtcm::set_DF005(bool sync_flag)
{
    // 0 - No further GNSS observables referenced to the same Epoch Time will be transmitted. This enables the receiver to begin processing
    //     the data immediately after decoding the message.
    // 1 - The next message will contain observables of another GNSS source referenced to the same Epoch Time.
    DF005 = std::bitset<1>(sync_flag);
    return 0;
}


int32_t Rtcm::set_DF006(const std::map<int32_t, Gnss_Synchro>& observables)
{
    // Number of satellites observed in current epoch
    uint16_t nsats = 0;
    std::map<int32_t, Gnss_Synchro>::const_iterator observables_iter;
    for (observables_iter = observables.cbegin();
        observables_iter != observables.cend();
        observables_iter++)
        {
            nsats++;
        }
    if (nsats > 31)
        {
            LOG(WARNING) << "The number of processed GPS satellites must be between 0 and 31, but it seems that you are processing " << nsats;
            nsats = 31;
        }
    DF006 = std::bitset<5>(nsats);
    return 0;
}


int32_t Rtcm::set_DF007(bool divergence_free_smoothing_indicator)
{
    // 0 - Divergence-free smoothing not used 1 - Divergence-free smoothing used
    DF007 = std::bitset<1>(divergence_free_smoothing_indicator);
    return 0;
}


int32_t Rtcm::set_DF008(int16_t smoothing_interval)
{
    DF008 = std::bitset<3>(smoothing_interval);
    return 0;
}


int32_t Rtcm::set_DF009(const Gnss_Synchro& gnss_synchro)
{
    const uint32_t prn_ = gnss_synchro.PRN;
    if (prn_ > 32)
        {
            LOG(WARNING) << "GPS satellite ID must be between 1 and 32, but PRN " << prn_ << " was found";
        }
    DF009 = std::bitset<6>(prn_);
    return 0;
}


int32_t Rtcm::set_DF009(const Gps_Ephemeris& gps_eph)
{
    const uint32_t prn_ = gps_eph.PRN;
    if (prn_ > 32)
        {
            LOG(WARNING) << "GPS satellite ID must be between 1 and 32, but PRN " << prn_ << " was found";
        }
    DF009 = std::bitset<6>(prn_);
    return 0;
}


int32_t Rtcm::set_DF010(bool code_indicator)
{
    DF010 = std::bitset<1>(code_indicator);
    return 0;
}


int32_t Rtcm::set_DF011(const Gnss_Synchro& gnss_synchro)
{
    const double ambiguity = std::floor(gnss_synchro.Pseudorange_m / 299792.458);
    const auto gps_L1_pseudorange = static_cast<uint64_t>(std::round((gnss_synchro.Pseudorange_m - ambiguity * 299792.458) / 0.02));
    DF011 = std::bitset<24>(gps_L1_pseudorange);
    return 0;
}


int32_t Rtcm::set_DF012(const Gnss_Synchro& gnss_synchro)
{
    const double lambda = SPEED_OF_LIGHT_M_S / GPS_L1_FREQ_HZ;
    const double ambiguity = std::floor(gnss_synchro.Pseudorange_m / 299792.458);
    const double gps_L1_pseudorange = std::round((gnss_synchro.Pseudorange_m - ambiguity * 299792.458) / 0.02);
    const double gps_L1_pseudorange_c = gps_L1_pseudorange * 0.02 + ambiguity * 299792.458;
    const double L1_phaserange_c = gnss_synchro.Carrier_phase_rads / TWO_PI;
    const double L1_phaserange_c_r = std::fmod(L1_phaserange_c - gps_L1_pseudorange_c / lambda + 1500.0, 3000.0) - 1500.0;
    const auto gps_L1_phaserange_minus_L1_pseudorange = static_cast<int64_t>(std::round(L1_phaserange_c_r * lambda / 0.0005));
    DF012 = std::bitset<20>(gps_L1_phaserange_minus_L1_pseudorange);
    return 0;
}


int32_t Rtcm::set_DF013(const Gps_Ephemeris& eph, double obs_time, const Gnss_Synchro& gnss_synchro)
{
    const uint32_t lock_time_period_s = Rtcm::lock_time(eph, obs_time, gnss_synchro);
    const uint32_t lock_time_indicator = Rtcm::lock_time_indicator(lock_time_period_s);
    DF013 = std::bitset<7>(lock_time_indicator);
    return 0;
}


int32_t Rtcm::set_DF014(const Gnss_Synchro& gnss_synchro)
{
    const auto gps_L1_pseudorange_ambiguity = static_cast<uint32_t>(std::floor(gnss_synchro.Pseudorange_m / 299792.458));
    DF014 = std::bitset<8>(gps_L1_pseudorange_ambiguity);
    return 0;
}


int32_t Rtcm::set_DF015(const Gnss_Synchro& gnss_synchro)
{
    double CN0_dB_Hz_est = gnss_synchro.CN0_dB_hz;
    if (CN0_dB_Hz_est > 63.75)
        {
            CN0_dB_Hz_est = 63.75;
        }
    const auto CN0_dB_Hz = static_cast<uint32_t>(std::round(CN0_dB_Hz_est / 0.25));
    DF015 = std::bitset<8>(CN0_dB_Hz);
    return 0;
}


int32_t Rtcm::set_DF017(const Gnss_Synchro& gnss_synchroL1, const Gnss_Synchro& gnss_synchroL2)
{
    const double ambiguity = std::floor(gnss_synchroL1.Pseudorange_m / 299792.458);
    const double gps_L1_pseudorange = std::round((gnss_synchroL1.Pseudorange_m - ambiguity * 299792.458) / 0.02);
    const double gps_L1_pseudorange_c = gps_L1_pseudorange * 0.02 + ambiguity * 299792.458;

    const double l2_l1_pseudorange = gnss_synchroL2.Pseudorange_m - gps_L1_pseudorange_c;
    int32_t pseudorange_difference = 0xFFFFE000;  // invalid value;
    if (std::fabs(l2_l1_pseudorange) <= 163.82)
        {
            pseudorange_difference = static_cast<int32_t>(std::round(l2_l1_pseudorange / 0.02));
        }
    DF017 = std::bitset<14>(pseudorange_difference);
    return 0;
}


int32_t Rtcm::set_DF018(const Gnss_Synchro& gnss_synchroL1, const Gnss_Synchro& gnss_synchroL2)
{
    const double lambda2 = SPEED_OF_LIGHT_M_S / GPS_L2_FREQ_HZ;
    int32_t l2_phaserange_minus_l1_pseudorange = 0xFFF80000;
    const double ambiguity = std::floor(gnss_synchroL1.Pseudorange_m / 299792.458);
    const double gps_L1_pseudorange = std::round((gnss_synchroL1.Pseudorange_m - ambiguity * 299792.458) / 0.02);
    const double gps_L1_pseudorange_c = gps_L1_pseudorange * 0.02 + ambiguity * 299792.458;
    const double L2_phaserange_c = gnss_synchroL2.Carrier_phase_rads / TWO_PI;
    const double L1_phaserange_c_r = std::fmod(L2_phaserange_c - gps_L1_pseudorange_c / lambda2 + 1500.0, 3000.0) - 1500.0;

    if (std::fabs(L1_phaserange_c_r * lambda2) <= 262.1435)
        {
            l2_phaserange_minus_l1_pseudorange = static_cast<int32_t>(std::round(L1_phaserange_c_r * lambda2 / 0.0005));
        }

    DF018 = std::bitset<20>(l2_phaserange_minus_l1_pseudorange);
    return 0;
}


int32_t Rtcm::set_DF019(const Gps_CNAV_Ephemeris& eph, double obs_time, const Gnss_Synchro& gnss_synchro)
{
    const uint32_t lock_time_period_s = Rtcm::lock_time(eph, obs_time, gnss_synchro);
    const uint32_t lock_time_indicator = Rtcm::lock_time_indicator(lock_time_period_s);
    DF019 = std::bitset<7>(lock_time_indicator);
    return 0;
}


int32_t Rtcm::set_DF020(const Gnss_Synchro& gnss_synchro)
{
    double CN0_dB_Hz_est = gnss_synchro.CN0_dB_hz;
    if (CN0_dB_Hz_est > 63.75)
        {
            CN0_dB_Hz_est = 63.75;
        }
    const auto CN0_dB_Hz = static_cast<uint32_t>(std::round(CN0_dB_Hz_est / 0.25));
    DF020 = std::bitset<8>(CN0_dB_Hz);
    return 0;
}

int32_t Rtcm::set_DF021()
{
    const uint16_t itfr_year = 0;
    DF021 = std::bitset<6>(itfr_year);
    return 0;
}


int32_t Rtcm::set_DF022(bool gps_indicator)
{
    DF022 = std::bitset<1>(gps_indicator);
    return 0;
}


int32_t Rtcm::set_DF023(bool glonass_indicator)
{
    DF023 = std::bitset<1>(glonass_indicator);
    return 0;
}


int32_t Rtcm::set_DF024(bool galileo_indicator)
{
    DF024 = std::bitset<1>(galileo_indicator);
    return 0;
}


int32_t Rtcm::set_DF025(double antenna_ECEF_X_m)
{
    const auto ant_ref_x = static_cast<int64_t>(std::round(antenna_ECEF_X_m * 10000));
    DF025 = std::bitset<38>(ant_ref_x);
    return 0;
}


int32_t Rtcm::set_DF026(double antenna_ECEF_Y_m)
{
    const auto ant_ref_y = static_cast<int64_t>(std::round(antenna_ECEF_Y_m * 10000));
    DF026 = std::bitset<38>(ant_ref_y);
    return 0;
}


int32_t Rtcm::set_DF027(double antenna_ECEF_Z_m)
{
    const auto ant_ref_z = static_cast<int64_t>(std::round(antenna_ECEF_Z_m * 10000));
    DF027 = std::bitset<38>(ant_ref_z);
    return 0;
}


int32_t Rtcm::set_DF028(double height)
{
    const auto h_ = static_cast<uint32_t>(std::round(height * 10000));
    DF028 = std::bitset<16>(h_);
    return 0;
}


int32_t Rtcm::set_DF031(uint32_t antenna_setup_id)
{
    DF031 = std::bitset<8>(antenna_setup_id);
    return 0;
}


int32_t Rtcm::set_DF034(double obs_time)
{
    // TOW in milliseconds from the beginning of the GLONASS day, measured in GLONASS time
    auto tk = static_cast<uint64_t>(std::round(obs_time * 1000));
    if (tk > 86400999)
        {
            LOG(WARNING) << "To large GLONASS Epoch Time (tk)! Set to the last millisecond of the day";
            tk = 86400999;
        }
    DF034 = std::bitset<27>(tk);
    return 0;
}


int32_t Rtcm::set_DF035(const std::map<int32_t, Gnss_Synchro>& observables)
{
    // Number of satellites observed in current epoch
    uint16_t nsats = 0;
    std::map<int32_t, Gnss_Synchro>::const_iterator observables_iter;
    for (observables_iter = observables.begin();
        observables_iter != observables.end();
        observables_iter++)
        {
            nsats++;
        }
    if (nsats > 31)
        {
            LOG(WARNING) << "The number of processed GLONASS satellites must be between 0 and 31, but it seems that you are processing " << nsats;
            nsats = 31;
        }
    DF035 = std::bitset<5>(nsats);
    return 0;
}


int32_t Rtcm::set_DF036(bool divergence_free_smoothing_indicator)
{
    // 0 - Divergence-free smoothing not used 1 - Divergence-free smoothing used
    DF036 = std::bitset<1>(divergence_free_smoothing_indicator);
    return 0;
}


int32_t Rtcm::set_DF037(int16_t smoothing_interval)
{
    DF037 = std::bitset<3>(smoothing_interval);
    return 0;
}


int32_t Rtcm::set_DF038(const Gnss_Synchro& gnss_synchro)
{
    const uint32_t prn_ = gnss_synchro.PRN;
    if (prn_ > 24)
        {
            LOG(WARNING) << "GLONASS satellite ID (Slot Number) must be between 1 and 24, but PRN " << prn_ << " was found";
        }
    DF038 = std::bitset<6>(prn_);
    return 0;
}


int32_t Rtcm::set_DF038(const Glonass_Gnav_Ephemeris& glonass_gnav_eph)
{
    const uint32_t prn_ = glonass_gnav_eph.i_satellite_slot_number;
    if (prn_ > 24)
        {
            LOG(WARNING) << "GLONASS satellite ID (Slot Number) must be between 0 and 24, but PRN " << prn_ << " was found";
        }
    DF038 = std::bitset<6>(prn_);
    return 0;
}


int32_t Rtcm::set_DF039(bool code_indicator)
{
    DF039 = std::bitset<1>(code_indicator);
    return 0;
}


int32_t Rtcm::set_DF040(int32_t frequency_channel_number)
{
    const uint32_t freq_ = frequency_channel_number + 7;
    if (freq_ > 20)
        {
            LOG(WARNING) << "GLONASS Satellite Frequency Number Conversion Error."
                         << "Value must be between 0 and 20, but converted channel"
                         << "frequency number " << freq_ << " was found";
        }

    DF040 = std::bitset<5>(freq_);
    return 0;
}


int32_t Rtcm::set_DF040(const Glonass_Gnav_Ephemeris& glonass_gnav_eph)
{
    const uint32_t freq_ = glonass_gnav_eph.i_satellite_freq_channel + 7;
    if (freq_ > 20)
        {
            LOG(WARNING) << "GLONASS Satellite Frequency Number Conversion Error."
                         << "Value must be between 0 and 20, but converted channel"
                         << "frequency number " << freq_ << " was found";
        }

    DF040 = std::bitset<5>(freq_);
    return 0;
}


int32_t Rtcm::set_DF041(const Gnss_Synchro& gnss_synchro)
{
    const double ambiguity = std::floor(gnss_synchro.Pseudorange_m / 599584.92);
    const auto glonass_L1_pseudorange = static_cast<uint64_t>(std::round((gnss_synchro.Pseudorange_m - ambiguity * 599584.92) / 0.02));
    DF041 = std::bitset<25>(glonass_L1_pseudorange);
    return 0;
}


int32_t Rtcm::set_DF042(const Gnss_Synchro& gnss_synchro)
{
    const double lambda = SPEED_OF_LIGHT_M_S / (GLONASS_L1_CA_FREQ_HZ + (GLONASS_L1_CA_DFREQ_HZ * GLONASS_PRN.at(gnss_synchro.PRN)));
    const double ambiguity = std::floor(gnss_synchro.Pseudorange_m / 599584.92);
    const double glonass_L1_pseudorange = std::round((gnss_synchro.Pseudorange_m - ambiguity * 599584.92) / 0.02);
    const double glonass_L1_pseudorange_c = glonass_L1_pseudorange * 0.02 + ambiguity * 299792.458;
    const double L1_phaserange_c = gnss_synchro.Carrier_phase_rads / TWO_PI;
    const double L1_phaserange_c_r = std::fmod(L1_phaserange_c - glonass_L1_pseudorange_c / lambda + 1500.0, 3000.0) - 1500.0;
    const auto glonass_L1_phaserange_minus_L1_pseudorange = static_cast<int64_t>(std::round(L1_phaserange_c_r * lambda / 0.0005));
    DF042 = std::bitset<20>(glonass_L1_phaserange_minus_L1_pseudorange);
    return 0;
}


int32_t Rtcm::set_DF043(const Glonass_Gnav_Ephemeris& eph, double obs_time, const Gnss_Synchro& gnss_synchro)
{
    const uint32_t lock_time_period_s = Rtcm::lock_time(eph, obs_time, gnss_synchro);
    const uint32_t lock_time_indicator = Rtcm::lock_time_indicator(lock_time_period_s);
    DF043 = std::bitset<7>(lock_time_indicator);
    return 0;
}


int32_t Rtcm::set_DF044(const Gnss_Synchro& gnss_synchro)
{
    const auto glonass_L1_pseudorange_ambiguity = static_cast<uint32_t>(std::floor(gnss_synchro.Pseudorange_m / 599584.916));
    DF044 = std::bitset<7>(glonass_L1_pseudorange_ambiguity);
    return 0;
}


int32_t Rtcm::set_DF045(const Gnss_Synchro& gnss_synchro)
{
    double CN0_dB_Hz_est = gnss_synchro.CN0_dB_hz;
    if (CN0_dB_Hz_est > 63.75)
        {
            LOG(WARNING) << "GLONASS L1 CNR must be between 0 and 63.75, but CNR " << CN0_dB_Hz_est << " was found. Setting to 63.75 dB-Hz";
            CN0_dB_Hz_est = 63.75;
        }
    const auto CN0_dB_Hz = static_cast<uint32_t>(std::round(CN0_dB_Hz_est / 0.25));
    DF045 = std::bitset<8>(CN0_dB_Hz);
    return 0;
}


int32_t Rtcm::set_DF047(const Gnss_Synchro& gnss_synchroL1, const Gnss_Synchro& gnss_synchroL2)
{
    const double ambiguity = std::floor(gnss_synchroL1.Pseudorange_m / 599584.92);
    const double glonass_L1_pseudorange = std::round((gnss_synchroL1.Pseudorange_m - ambiguity * 599584.92) / 0.02);
    const double glonass_L1_pseudorange_c = glonass_L1_pseudorange * 0.02 + ambiguity * 599584.92;

    const double l2_l1_pseudorange = gnss_synchroL2.Pseudorange_m - glonass_L1_pseudorange_c;
    int32_t pseudorange_difference = 0xFFFFE000;  // invalid value;
    if (std::fabs(l2_l1_pseudorange) <= 163.82)
        {
            pseudorange_difference = static_cast<int32_t>(std::round(l2_l1_pseudorange / 0.02));
        }
    DF047 = std::bitset<14>(pseudorange_difference);
    return 0;
}

// TODO Need to consider frequency channel in this fields
int32_t Rtcm::set_DF048(const Gnss_Synchro& gnss_synchroL1, const Gnss_Synchro& gnss_synchroL2)
{
    const double lambda2 = SPEED_OF_LIGHT_M_S / GLONASS_L2_CA_FREQ_HZ;
    int32_t l2_phaserange_minus_l1_pseudorange = 0xFFF80000;
    const double ambiguity = std::floor(gnss_synchroL1.Pseudorange_m / 599584.92);
    const double glonass_L1_pseudorange = std::round((gnss_synchroL1.Pseudorange_m - ambiguity * 599584.92) / 0.02);
    const double glonass_L1_pseudorange_c = glonass_L1_pseudorange * 0.02 + ambiguity * 599584.92;
    const double L2_phaserange_c = gnss_synchroL2.Carrier_phase_rads / TWO_PI;
    const double L1_phaserange_c_r = std::fmod(L2_phaserange_c - glonass_L1_pseudorange_c / lambda2 + 1500.0, 3000.0) - 1500.0;

    if (std::fabs(L1_phaserange_c_r * lambda2) <= 262.1435)
        {
            l2_phaserange_minus_l1_pseudorange = static_cast<int32_t>(std::round(L1_phaserange_c_r * lambda2 / 0.0005));
        }

    DF048 = std::bitset<20>(l2_phaserange_minus_l1_pseudorange);
    return 0;
}


int32_t Rtcm::set_DF049(const Glonass_Gnav_Ephemeris& eph, double obs_time, const Gnss_Synchro& gnss_synchro)
{
    const uint32_t lock_time_period_s = Rtcm::lock_time(eph, obs_time, gnss_synchro);
    const uint32_t lock_time_indicator = Rtcm::lock_time_indicator(lock_time_period_s);
    DF049 = std::bitset<7>(lock_time_indicator);
    return 0;
}


int32_t Rtcm::set_DF050(const Gnss_Synchro& gnss_synchro)
{
    double CN0_dB_Hz_est = gnss_synchro.CN0_dB_hz;
    if (CN0_dB_Hz_est > 63.75)
        {
            CN0_dB_Hz_est = 63.75;
        }
    const auto CN0_dB_Hz = static_cast<uint32_t>(std::round(CN0_dB_Hz_est / 0.25));
    DF050 = std::bitset<8>(CN0_dB_Hz);
    return 0;
}


int32_t Rtcm::set_DF051(const Gps_Ephemeris& gps_eph, double obs_time)
{
    const double gps_t = obs_time;
    const boost::posix_time::time_duration t_duration = boost::posix_time::milliseconds(static_cast<int64_t>((gps_t + 604800 * static_cast<double>(gps_eph.WN)) * 1000));
    std::string now_ptime;
    if (gps_eph.WN < 512)
        {
            boost::posix_time::ptime p_time(boost::gregorian::date(2019, 4, 7), t_duration);
            now_ptime = to_iso_string(p_time);
        }
    else
        {
            boost::posix_time::ptime p_time(boost::gregorian::date(1999, 8, 22), t_duration);
            now_ptime = to_iso_string(p_time);
        }
    const std::string today_ptime = now_ptime.substr(0, 8);
    boost::gregorian::date d(boost::gregorian::from_undelimited_string(today_ptime));
    uint32_t mjd = d.modjulian_day();
    DF051 = std::bitset<16>(mjd);
    return 0;
}


int32_t Rtcm::set_DF052(const Gps_Ephemeris& gps_eph, double obs_time)
{
    const double gps_t = obs_time;
    const boost::posix_time::time_duration t_duration = boost::posix_time::milliseconds(static_cast<int64_t>((gps_t + 604800 * static_cast<double>(gps_eph.WN)) * 1000));
    std::string now_ptime;
    if (gps_eph.WN < 512)
        {
            boost::posix_time::ptime p_time(boost::gregorian::date(2019, 4, 7), t_duration);
            now_ptime = to_iso_string(p_time);
        }
    else
        {
            boost::posix_time::ptime p_time(boost::gregorian::date(1999, 8, 22), t_duration);
            now_ptime = to_iso_string(p_time);
        }
    const std::string hours = now_ptime.substr(9, 2);
    const std::string minutes = now_ptime.substr(11, 2);
    const std::string seconds = now_ptime.substr(13, 8);
    // boost::gregorian::date d(boost::gregorian::from_undelimited_string(today_ptime));
    uint32_t seconds_of_day = boost::lexical_cast<uint32_t>(hours) * 60 * 60 + boost::lexical_cast<uint32_t>(minutes) * 60 + boost::lexical_cast<uint32_t>(seconds);
    DF052 = std::bitset<17>(seconds_of_day);
    return 0;
}


int32_t Rtcm::set_DF071(const Gps_Ephemeris& gps_eph)
{
    const auto iode = static_cast<uint32_t>(gps_eph.IODE_SF2);
    DF071 = std::bitset<8>(iode);
    return 0;
}


int32_t Rtcm::set_DF076(const Gps_Ephemeris& gps_eph)
{
    const auto week_number = static_cast<uint32_t>(gps_eph.WN);
    DF076 = std::bitset<10>(week_number);
    return 0;
}


int32_t Rtcm::set_DF077(const Gps_Ephemeris& gps_eph)
{
    const auto ura = static_cast<uint16_t>(gps_eph.SV_accuracy);
    DF077 = std::bitset<4>(ura);
    return 0;
}


int32_t Rtcm::set_DF078(const Gps_Ephemeris& gps_eph)
{
    const auto code_on_L2 = static_cast<uint16_t>(gps_eph.code_on_L2);
    DF078 = std::bitset<2>(code_on_L2);
    return 0;
}


int32_t Rtcm::set_DF079(const Gps_Ephemeris& gps_eph)
{
    const auto idot = static_cast<uint32_t>(std::round(gps_eph.idot / I_DOT_LSB));
    DF079 = std::bitset<14>(idot);
    return 0;
}


int32_t Rtcm::set_DF080(const Gps_Ephemeris& gps_eph)
{
    const auto iode = static_cast<uint16_t>(gps_eph.IODE_SF2);
    DF080 = std::bitset<8>(iode);
    return 0;
}


int32_t Rtcm::set_DF081(const Gps_Ephemeris& gps_eph)
{
    const auto toc = static_cast<uint32_t>(std::round(gps_eph.toc / T_OC_LSB));
    DF081 = std::bitset<16>(toc);
    return 0;
}


int32_t Rtcm::set_DF082(const Gps_Ephemeris& gps_eph)
{
    const auto af2 = static_cast<int16_t>(std::round(gps_eph.af2 / A_F2_LSB));
    DF082 = std::bitset<8>(af2);
    return 0;
}


int32_t Rtcm::set_DF083(const Gps_Ephemeris& gps_eph)
{
    const auto af1 = static_cast<int32_t>(std::round(gps_eph.af1 / A_F1_LSB));
    DF083 = std::bitset<16>(af1);
    return 0;
}


int32_t Rtcm::set_DF084(const Gps_Ephemeris& gps_eph)
{
    const auto af0 = static_cast<int64_t>(std::round(gps_eph.af0 / A_F0_LSB));
    DF084 = std::bitset<22>(af0);
    return 0;
}


int32_t Rtcm::set_DF085(const Gps_Ephemeris& gps_eph)
{
    const auto iodc = static_cast<uint32_t>(gps_eph.IODC);
    DF085 = std::bitset<10>(iodc);
    return 0;
}


int32_t Rtcm::set_DF086(const Gps_Ephemeris& gps_eph)
{
    const auto crs = static_cast<int32_t>(std::round(gps_eph.Crs / C_RS_LSB));
    DF086 = std::bitset<16>(crs);
    return 0;
}


int32_t Rtcm::set_DF087(const Gps_Ephemeris& gps_eph)
{
    const auto delta_n = static_cast<int32_t>(std::round(gps_eph.delta_n / DELTA_N_LSB));
    DF087 = std::bitset<16>(delta_n);
    return 0;
}


int32_t Rtcm::set_DF088(const Gps_Ephemeris& gps_eph)
{
    const auto m0 = static_cast<int64_t>(std::round(gps_eph.M_0 / M_0_LSB));
    DF088 = std::bitset<32>(m0);
    return 0;
}


int32_t Rtcm::set_DF089(const Gps_Ephemeris& gps_eph)
{
    const auto cuc = static_cast<int32_t>(std::round(gps_eph.Cuc / C_UC_LSB));
    DF089 = std::bitset<16>(cuc);
    return 0;
}

int32_t Rtcm::set_DF090(const Gps_Ephemeris& gps_eph)
{
    const auto ecc = static_cast<uint64_t>(std::round(gps_eph.ecc / ECCENTRICITY_LSB));
    DF090 = std::bitset<32>(ecc);
    return 0;
}


int32_t Rtcm::set_DF091(const Gps_Ephemeris& gps_eph)
{
    const auto cus = static_cast<int32_t>(std::round(gps_eph.Cus / C_US_LSB));
    DF091 = std::bitset<16>(cus);
    return 0;
}


int32_t Rtcm::set_DF092(const Gps_Ephemeris& gps_eph)
{
    const auto sqr_a = static_cast<uint64_t>(std::round(gps_eph.sqrtA / SQRT_A_LSB));
    DF092 = std::bitset<32>(sqr_a);
    return 0;
}


int32_t Rtcm::set_DF093(const Gps_Ephemeris& gps_eph)
{
    const auto toe = static_cast<uint32_t>(std::round(gps_eph.toe / T_OE_LSB));
    DF093 = std::bitset<16>(toe);
    return 0;
}


int32_t Rtcm::set_DF094(const Gps_Ephemeris& gps_eph)
{
    const auto cic = static_cast<int32_t>(std::round(gps_eph.Cic / C_IC_LSB));
    DF094 = std::bitset<16>(cic);
    return 0;
}


int32_t Rtcm::set_DF095(const Gps_Ephemeris& gps_eph)
{
    const auto Omega0 = static_cast<int64_t>(std::round(gps_eph.OMEGA_0 / OMEGA_0_LSB));
    DF095 = std::bitset<32>(Omega0);
    return 0;
}


int32_t Rtcm::set_DF096(const Gps_Ephemeris& gps_eph)
{
    const auto cis = static_cast<int32_t>(std::round(gps_eph.Cis / C_IS_LSB));
    DF096 = std::bitset<16>(cis);
    return 0;
}


int32_t Rtcm::set_DF097(const Gps_Ephemeris& gps_eph)
{
    const auto i0 = static_cast<int64_t>(std::round(gps_eph.i_0 / I_0_LSB));
    DF097 = std::bitset<32>(i0);
    return 0;
}


int32_t Rtcm::set_DF098(const Gps_Ephemeris& gps_eph)
{
    const auto crc = static_cast<int32_t>(std::round(gps_eph.Crc / C_RC_LSB));
    DF098 = std::bitset<16>(crc);
    return 0;
}


int32_t Rtcm::set_DF099(const Gps_Ephemeris& gps_eph)
{
    const auto omega = static_cast<int64_t>(std::round(gps_eph.omega / OMEGA_LSB));
    DF099 = std::bitset<32>(omega);
    return 0;
}


int32_t Rtcm::set_DF100(const Gps_Ephemeris& gps_eph)
{
    const auto omegadot = static_cast<int64_t>(std::round(gps_eph.OMEGAdot / OMEGA_DOT_LSB));
    DF100 = std::bitset<24>(omegadot);
    return 0;
}


int32_t Rtcm::set_DF101(const Gps_Ephemeris& gps_eph)
{
    const auto tgd = static_cast<int16_t>(std::round(gps_eph.TGD / T_GD_LSB));
    DF101 = std::bitset<8>(tgd);
    return 0;
}


int32_t Rtcm::set_DF102(const Gps_Ephemeris& gps_eph)
{
    const auto sv_heath = static_cast<uint16_t>(gps_eph.SV_health);
    DF102 = std::bitset<6>(sv_heath);
    return 0;
}


int32_t Rtcm::set_DF103(const Gps_Ephemeris& gps_eph)
{
    DF103 = std::bitset<1>(gps_eph.L2_P_data_flag);
    return 0;
}


int32_t Rtcm::set_DF104(uint32_t glonass_gnav_alm_health)
{
    DF104 = std::bitset<1>(glonass_gnav_alm_health);
    return 0;
}


int32_t Rtcm::set_DF105(uint32_t glonass_gnav_alm_health_ind)
{
    DF105 = std::bitset<1>(glonass_gnav_alm_health_ind);
    return 0;
}


int32_t Rtcm::set_DF106(const Glonass_Gnav_Ephemeris& glonass_gnav_eph)
{
    // Convert the value from (0, 30, 45, 60) to (00, 01, 10, 11)
    uint32_t P_1_tmp = std::round(glonass_gnav_eph.d_P_1 / 15.);
    uint32_t P_1 = (P_1_tmp == 0) ? 0 : P_1_tmp - 1;
    DF106 = std::bitset<2>(P_1);
    return 0;
}


int32_t Rtcm::set_DF107(const Glonass_Gnav_Ephemeris& glonass_gnav_eph)
{
    uint32_t hrs = 0;
    uint32_t min = 0;
    uint32_t sec = 0;
    uint32_t tk = 0;
    tk = static_cast<int32_t>(glonass_gnav_eph.d_t_k);
    hrs = tk / 3600;
    min = (tk - hrs * 3600) / 60;
    sec = (tk - hrs * 3600 - min * 60) / 60;

    std::string _hrs = std::bitset<5>(hrs).to_string();  // string conversion
    std::string _min = std::bitset<6>(min).to_string();  // string conversion
    std::string _sec = std::bitset<1>(sec).to_string();  // string conversion

    // Set hrs, min, sec in designed bit positions
    DF107 = std::bitset<12>(_hrs + _min + _sec);

    return 0;
}


int32_t Rtcm::set_DF108(const Glonass_Gnav_Ephemeris& glonass_gnav_eph)
{
    DF108 = std::bitset<1>(static_cast<bool>(glonass_gnav_eph.d_B_n));
    return 0;
}


int32_t Rtcm::set_DF109(const Glonass_Gnav_Ephemeris& glonass_gnav_eph)
{
    DF109 = std::bitset<1>(static_cast<bool>(glonass_gnav_eph.d_P_2));
    return 0;
}


int32_t Rtcm::set_DF110(const Glonass_Gnav_Ephemeris& glonass_gnav_eph)
{
    const auto t_b = static_cast<uint32_t>(std::round(glonass_gnav_eph.d_t_b / (15 * 60)));
    DF110 = std::bitset<7>(t_b);
    return 0;
}


int32_t Rtcm::set_DF111(const Glonass_Gnav_Ephemeris& glonass_gnav_eph)
{
    const auto VXn_mag = static_cast<int32_t>(std::round(fabs(glonass_gnav_eph.d_VXn / TWO_N20)));
    const uint32_t VXn_sgn = glo_sgn(glonass_gnav_eph.d_VXn);

    DF111 = std::bitset<24>(VXn_mag);
    DF111.set(23, VXn_sgn);
    return 0;
}


int32_t Rtcm::set_DF112(const Glonass_Gnav_Ephemeris& glonass_gnav_eph)
{
    const auto Xn_mag = static_cast<int32_t>(std::round(fabs(glonass_gnav_eph.d_Xn / TWO_N11)));
    const uint32_t Xn_sgn = glo_sgn(glonass_gnav_eph.d_Xn);

    DF112 = std::bitset<27>(Xn_mag);
    DF112.set(26, Xn_sgn);
    return 0;
}


int32_t Rtcm::set_DF113(const Glonass_Gnav_Ephemeris& glonass_gnav_eph)
{
    const auto AXn_mag = static_cast<int32_t>(std::round(fabs(glonass_gnav_eph.d_AXn / TWO_N30)));
    const uint32_t AXn_sgn = glo_sgn(glonass_gnav_eph.d_AXn);

    DF113 = std::bitset<5>(AXn_mag);
    DF113.set(4, AXn_sgn);
    return 0;
}


int32_t Rtcm::set_DF114(const Glonass_Gnav_Ephemeris& glonass_gnav_eph)
{
    const auto VYn_mag = static_cast<int32_t>(std::round(fabs(glonass_gnav_eph.d_VYn / TWO_N20)));
    const uint32_t VYn_sgn = glo_sgn(glonass_gnav_eph.d_VYn);

    DF114 = std::bitset<24>(VYn_mag);
    DF114.set(23, VYn_sgn);
    return 0;
}


int32_t Rtcm::set_DF115(const Glonass_Gnav_Ephemeris& glonass_gnav_eph)
{
    const auto Yn_mag = static_cast<int32_t>(std::round(fabs(glonass_gnav_eph.d_Yn / TWO_N11)));
    const uint32_t Yn_sgn = glo_sgn(glonass_gnav_eph.d_Yn);

    DF115 = std::bitset<27>(Yn_mag);
    DF115.set(26, Yn_sgn);
    return 0;
}


int32_t Rtcm::set_DF116(const Glonass_Gnav_Ephemeris& glonass_gnav_eph)
{
    const auto AYn_mag = static_cast<int32_t>(std::round(fabs(glonass_gnav_eph.d_AYn / TWO_N30)));
    const uint32_t AYn_sgn = glo_sgn(glonass_gnav_eph.d_AYn);

    DF116 = std::bitset<5>(AYn_mag);
    DF116.set(4, AYn_sgn);
    return 0;
}


int32_t Rtcm::set_DF117(const Glonass_Gnav_Ephemeris& glonass_gnav_eph)
{
    const auto VZn_mag = static_cast<int32_t>(std::round(fabs(glonass_gnav_eph.d_VZn / TWO_N20)));
    const uint32_t VZn_sgn = glo_sgn(glonass_gnav_eph.d_VZn);

    DF117 = std::bitset<24>(VZn_mag);
    DF117.set(23, VZn_sgn);
    return 0;
}


int32_t Rtcm::set_DF118(const Glonass_Gnav_Ephemeris& glonass_gnav_eph)
{
    const auto Zn_mag = static_cast<int32_t>(std::round(fabs(glonass_gnav_eph.d_Zn / TWO_N11)));
    const uint32_t Zn_sgn = glo_sgn(glonass_gnav_eph.d_Zn);

    DF118 = std::bitset<27>(Zn_mag);
    DF118.set(26, Zn_sgn);
    return 0;
}


int32_t Rtcm::set_DF119(const Glonass_Gnav_Ephemeris& glonass_gnav_eph)
{
    const auto AZn_mag = static_cast<int32_t>(std::round(fabs(glonass_gnav_eph.d_AZn / TWO_N30)));
    const uint32_t AZn_sgn = glo_sgn(glonass_gnav_eph.d_AZn);

    DF119 = std::bitset<5>(AZn_mag);
    DF119.set(4, AZn_sgn);
    return 0;
}


int32_t Rtcm::set_DF120(const Glonass_Gnav_Ephemeris& glonass_gnav_eph)
{
    const auto P3_aux = static_cast<uint32_t>(std::round(glonass_gnav_eph.d_P_3));
    DF120 = std::bitset<1>(P3_aux);
    return 0;
}


int32_t Rtcm::set_DF121(const Glonass_Gnav_Ephemeris& glonass_gnav_eph)
{
    const auto gamma_mag = static_cast<int32_t>(std::round(fabs(glonass_gnav_eph.d_gamma_n / TWO_N40)));
    const uint32_t gamma_sgn = glo_sgn(glonass_gnav_eph.d_gamma_n);

    DF121 = std::bitset<11>(gamma_mag);
    DF121.set(10, gamma_sgn);
    return 0;
}


int32_t Rtcm::set_DF122(const Glonass_Gnav_Ephemeris& glonass_gnav_eph)
{
    const auto P_aux = static_cast<uint32_t>(std::round(glonass_gnav_eph.d_P));
    DF122 = std::bitset<2>(P_aux);
    return 0;
}


int32_t Rtcm::set_DF123(const Glonass_Gnav_Ephemeris& glonass_gnav_eph)
{
    const auto ln = static_cast<uint32_t>((glonass_gnav_eph.d_l3rd_n));
    DF123 = std::bitset<1>(ln);
    return 0;
}


int32_t Rtcm::set_DF124(const Glonass_Gnav_Ephemeris& glonass_gnav_eph)
{
    const auto tau_mag = static_cast<int32_t>(std::round(fabs(glonass_gnav_eph.d_tau_n / TWO_N30)));
    const uint32_t tau_sgn = glo_sgn(glonass_gnav_eph.d_tau_n);

    DF124 = std::bitset<22>(tau_mag);
    DF124.set(21, tau_sgn);
    return 0;
}


int32_t Rtcm::set_DF125(const Glonass_Gnav_Ephemeris& glonass_gnav_eph)
{
    const auto delta_tau_mag = static_cast<int32_t>(std::round(fabs(glonass_gnav_eph.d_Delta_tau_n / TWO_N30)));
    const uint32_t delta_tau_sgn = glo_sgn(glonass_gnav_eph.d_Delta_tau_n);

    DF125 = std::bitset<5>(delta_tau_mag);
    DF125.set(4, delta_tau_sgn);
    return 0;
}


int32_t Rtcm::set_DF126(const Glonass_Gnav_Ephemeris& glonass_gnav_eph)
{
    const auto ecc = static_cast<uint32_t>(std::round(glonass_gnav_eph.d_E_n));
    DF126 = std::bitset<5>(ecc);
    return 0;
}


int32_t Rtcm::set_DF127(const Glonass_Gnav_Ephemeris& glonass_gnav_eph)
{
    const auto P4_aux = static_cast<uint32_t>(std::round(glonass_gnav_eph.d_P_4));
    DF127 = std::bitset<1>(P4_aux);
    return 0;
}


int32_t Rtcm::set_DF128(const Glonass_Gnav_Ephemeris& glonass_gnav_eph)
{
    const auto F_t = static_cast<uint32_t>(std::round(glonass_gnav_eph.d_F_T));
    DF128 = std::bitset<4>(F_t);
    return 0;
}


int32_t Rtcm::set_DF129(const Glonass_Gnav_Ephemeris& glonass_gnav_eph)
{
    const auto N_t = static_cast<uint32_t>(std::round(glonass_gnav_eph.d_N_T));
    DF129 = std::bitset<11>(N_t);
    return 0;
}


int32_t Rtcm::set_DF130(const Glonass_Gnav_Ephemeris& glonass_gnav_eph)
{
    const auto M_aux = static_cast<uint32_t>(std::round(glonass_gnav_eph.d_M));
    DF130 = std::bitset<2>(M_aux);
    return 0;
}


int32_t Rtcm::set_DF131(uint32_t fifth_str_additional_data_ind)
{
    const auto fith_str_data = static_cast<uint32_t>(fifth_str_additional_data_ind);
    DF131 = std::bitset<1>(fith_str_data);
    return 0;
}


int32_t Rtcm::set_DF132(const Glonass_Gnav_Utc_Model& glonass_gnav_utc_model)
{
    const auto N_A_aux = static_cast<uint32_t>(std::round(glonass_gnav_utc_model.d_N_A));
    DF132 = std::bitset<11>(N_A_aux);
    return 0;
}


int32_t Rtcm::set_DF133(const Glonass_Gnav_Utc_Model& glonass_gnav_utc_model)
{
    const auto tau_c = static_cast<int32_t>(std::round(glonass_gnav_utc_model.d_tau_c / TWO_N31));
    DF133 = std::bitset<32>(tau_c);
    return 0;
}


int32_t Rtcm::set_DF134(const Glonass_Gnav_Utc_Model& glonass_gnav_utc_model)
{
    const auto N_4_aux = static_cast<uint32_t>(std::round(glonass_gnav_utc_model.d_N_4));
    DF134 = std::bitset<5>(N_4_aux);
    return 0;
}


int32_t Rtcm::set_DF135(const Glonass_Gnav_Utc_Model& glonass_gnav_utc_model)
{
    const auto tau_gps = static_cast<int32_t>(std::round(glonass_gnav_utc_model.d_tau_gps) / TWO_N30);
    DF135 = std::bitset<22>(tau_gps);
    return 0;
}


int32_t Rtcm::set_DF136(const Glonass_Gnav_Ephemeris& glonass_gnav_eph)
{
    const auto l_n = static_cast<uint32_t>(std::round(glonass_gnav_eph.d_l5th_n));
    DF136 = std::bitset<1>(l_n);
    return 0;
}


int32_t Rtcm::set_DF137(const Gps_Ephemeris& gps_eph)
{
    DF137 = std::bitset<1>(gps_eph.fit_interval_flag);
    return 0;
}


int32_t Rtcm::set_DF248(double obs_time)
{
    // TOW in milliseconds from the beginning of the Galileo week, measured in Galileo time
    auto tow = static_cast<uint64_t>(std::round(obs_time * 1000));
    if (tow > 604799999)
        {
            LOG(WARNING) << "To large TOW! Set to the last millisecond of the week";
            tow = 604799999;
        }
    DF248 = std::bitset<30>(tow);
    return 0;
}


int32_t Rtcm::set_DF252(const Galileo_Ephemeris& gal_eph)
{
    const uint32_t prn_ = gal_eph.PRN;
    if (prn_ > 63)
        {
            LOG(WARNING) << "Galileo satellite ID must be between 0 and 63, but PRN " << prn_ << " was found";
        }
    DF252 = std::bitset<6>(prn_);
    return 0;
}


int32_t Rtcm::set_DF289(const Galileo_Ephemeris& gal_eph)
{
    const auto galileo_week_number = static_cast<uint32_t>(gal_eph.WN);
    if (galileo_week_number > 4095)
        {
            LOG(WARNING) << "Error decoding Galileo week number (it has a 4096 roll-off, but " << galileo_week_number << " was detected)";
        }
    DF289 = std::bitset<12>(galileo_week_number);
    return 0;
}


int32_t Rtcm::set_DF290(const Galileo_Ephemeris& gal_eph)
{
    const auto iod_nav = static_cast<uint32_t>(gal_eph.IOD_nav);
    if (iod_nav > 1023)
        {
            LOG(WARNING) << "Error decoding Galileo IODnav (it has a max of 1023, but " << iod_nav << " was detected)";
        }
    DF290 = std::bitset<10>(iod_nav);
    return 0;
}


int32_t Rtcm::set_DF291(const Galileo_Ephemeris& gal_eph)
{
    const auto SISA = static_cast<uint16_t>(gal_eph.SISA);
    // SISA = 0; // SIS Accuracy, data content definition not given in Galileo OS SIS ICD, Issue 1.1, Sept 2010
    DF291 = std::bitset<8>(SISA);
    return 0;
}


int32_t Rtcm::set_DF292(const Galileo_Ephemeris& gal_eph)
{
    const auto idot = static_cast<int32_t>(std::round(gal_eph.idot / FNAV_IDOT_2_LSB));
    DF292 = std::bitset<14>(idot);
    return 0;
}


int32_t Rtcm::set_DF293(const Galileo_Ephemeris& gal_eph)
{
    const auto toc = static_cast<uint32_t>(gal_eph.toc);
    if (toc > 604740)
        {
            LOG(WARNING) << "Error decoding Galileo ephemeris time (max of 604740, but " << toc << " was detected)";
        }
    DF293 = std::bitset<14>(toc);
    return 0;
}


int32_t Rtcm::set_DF294(const Galileo_Ephemeris& gal_eph)
{
    const auto af2 = static_cast<int16_t>(std::round(gal_eph.af2 / FNAV_AF2_1_LSB));
    DF294 = std::bitset<6>(af2);
    return 0;
}


int32_t Rtcm::set_DF295(const Galileo_Ephemeris& gal_eph)
{
    const auto af1 = static_cast<int64_t>(std::round(gal_eph.af1 / FNAV_AF1_1_LSB));
    DF295 = std::bitset<21>(af1);
    return 0;
}


int32_t Rtcm::set_DF296(const Galileo_Ephemeris& gal_eph)
{
    const int64_t af0 = static_cast<uint32_t>(std::round(gal_eph.af0 / FNAV_AF0_1_LSB));
    DF296 = std::bitset<31>(af0);
    return 0;
}


int32_t Rtcm::set_DF297(const Galileo_Ephemeris& gal_eph)
{
    const auto crs = static_cast<int32_t>(std::round(gal_eph.Crs / FNAV_CRS_3_LSB));
    DF297 = std::bitset<16>(crs);
    return 0;
}


int32_t Rtcm::set_DF298(const Galileo_Ephemeris& gal_eph)
{
    const auto delta_n = static_cast<int32_t>(std::round(gal_eph.delta_n / FNAV_DELTAN_3_LSB));
    DF298 = std::bitset<16>(delta_n);
    return 0;
}


int32_t Rtcm::set_DF299(const Galileo_Ephemeris& gal_eph)
{
    const auto m0 = static_cast<int64_t>(std::round(gal_eph.M_0 / FNAV_M0_2_LSB));
    DF299 = std::bitset<32>(m0);
    return 0;
}


int32_t Rtcm::set_DF300(const Galileo_Ephemeris& gal_eph)
{
    const int32_t cuc = static_cast<uint32_t>(std::round(gal_eph.Cuc / FNAV_CUC_3_LSB));
    DF300 = std::bitset<16>(cuc);
    return 0;
}


int32_t Rtcm::set_DF301(const Galileo_Ephemeris& gal_eph)
{
    const auto ecc = static_cast<uint64_t>(std::round(gal_eph.ecc / FNAV_E_2_LSB));
    DF301 = std::bitset<32>(ecc);
    return 0;
}


int32_t Rtcm::set_DF302(const Galileo_Ephemeris& gal_eph)
{
    const auto cus = static_cast<int32_t>(std::round(gal_eph.Cus / FNAV_CUS_3_LSB));
    DF302 = std::bitset<16>(cus);
    return 0;
}


int32_t Rtcm::set_DF303(const Galileo_Ephemeris& gal_eph)
{
    const auto sqr_a = static_cast<uint64_t>(std::round(gal_eph.sqrtA / FNAV_A12_2_LSB));
    DF303 = std::bitset<32>(sqr_a);
    return 0;
}


int32_t Rtcm::set_DF304(const Galileo_Ephemeris& gal_eph)
{
    const auto toe = static_cast<uint32_t>(std::round(gal_eph.toe / FNAV_T0E_3_LSB));
    DF304 = std::bitset<14>(toe);
    return 0;
}


int32_t Rtcm::set_DF305(const Galileo_Ephemeris& gal_eph)
{
    const auto cic = static_cast<int32_t>(std::round(gal_eph.Cic / FNAV_CIC_4_LSB));
    DF305 = std::bitset<16>(cic);
    return 0;
}


int32_t Rtcm::set_DF306(const Galileo_Ephemeris& gal_eph)
{
    const auto Omega0 = static_cast<int64_t>(std::round(gal_eph.OMEGA_0 / FNAV_OMEGA0_2_LSB));
    DF306 = std::bitset<32>(Omega0);
    return 0;
}


int32_t Rtcm::set_DF307(const Galileo_Ephemeris& gal_eph)
{
    const auto cis = static_cast<int32_t>(std::round(gal_eph.Cis / FNAV_CIS_4_LSB));
    DF307 = std::bitset<16>(cis);
    return 0;
}


int32_t Rtcm::set_DF308(const Galileo_Ephemeris& gal_eph)
{
    const auto i0 = static_cast<int64_t>(std::round(gal_eph.i_0 / FNAV_I0_3_LSB));
    DF308 = std::bitset<32>(i0);
    return 0;
}


int32_t Rtcm::set_DF309(const Galileo_Ephemeris& gal_eph)
{
    const int32_t crc = static_cast<uint32_t>(std::round(gal_eph.Crc / FNAV_CRC_3_LSB));
    DF309 = std::bitset<16>(crc);
    return 0;
}


int32_t Rtcm::set_DF310(const Galileo_Ephemeris& gal_eph)
{
    const auto omega = static_cast<int32_t>(std::round(gal_eph.omega / FNAV_OMEGA0_2_LSB));
    DF310 = std::bitset<32>(omega);
    return 0;
}


int32_t Rtcm::set_DF311(const Galileo_Ephemeris& gal_eph)
{
    const auto Omegadot = static_cast<int64_t>(std::round(gal_eph.OMEGAdot / FNAV_OMEGADOT_2_LSB));
    DF311 = std::bitset<24>(Omegadot);
    return 0;
}


int32_t Rtcm::set_DF312(const Galileo_Ephemeris& gal_eph)
{
    const auto bdg_E1_E5a = static_cast<int32_t>(std::round(gal_eph.BGD_E1E5a / FNAV_BGD_1_LSB));
    DF312 = std::bitset<10>(bdg_E1_E5a);
    return 0;
}


int32_t Rtcm::set_DF313(const Galileo_Ephemeris& gal_eph)
{
    const auto bdg_E5b_E1 = static_cast<uint32_t>(std::round(gal_eph.BGD_E1E5b));
    // bdg_E5b_E1 = 0; // reserved
    DF313 = std::bitset<10>(bdg_E5b_E1);
    return 0;
}


int32_t Rtcm::set_DF314(const Galileo_Ephemeris& gal_eph)
{
    DF314 = std::bitset<2>(gal_eph.E5a_HS);
    return 0;
}


int32_t Rtcm::set_DF315(const Galileo_Ephemeris& gal_eph)
{
    DF315 = std::bitset<1>(gal_eph.E5a_DVS);
    return 0;
}


int32_t Rtcm::set_DF393(bool more_messages)
{
    DF393 = std::bitset<1>(more_messages);
    return 0;
}


int32_t Rtcm::set_DF394(const std::map<int32_t, Gnss_Synchro>& gnss_synchro)
{
    DF394.reset();
    std::map<int32_t, Gnss_Synchro>::const_iterator gnss_synchro_iter;
    uint32_t mask_position;
    for (gnss_synchro_iter = gnss_synchro.cbegin();
        gnss_synchro_iter != gnss_synchro.cend();
        gnss_synchro_iter++)
        {
            mask_position = 64 - gnss_synchro_iter->second.PRN;
            DF394.set(mask_position, true);
        }
    return 0;
}


int32_t Rtcm::set_DF395(const std::map<int32_t, Gnss_Synchro>& gnss_synchro)
{
    DF395.reset();
    if (gnss_synchro.empty())
        {
            return 1;
        }
    std::map<int32_t, Gnss_Synchro>::const_iterator gnss_synchro_iter;
    std::string sig;
    uint32_t mask_position;
    for (gnss_synchro_iter = gnss_synchro.cbegin();
        gnss_synchro_iter != gnss_synchro.cend();
        gnss_synchro_iter++)
        {
            const std::string sig_(gnss_synchro_iter->second.Signal);
            sig = sig_.substr(0, 2);

            const std::string sys(&gnss_synchro_iter->second.System, 1);

            if ((sig == "1C") && (sys == "G"))
                {
                    mask_position = 32 - 2;
                    DF395.set(mask_position, true);
                }
            if ((sig == "2S") && (sys == "G"))
                {
                    mask_position = 32 - 15;
                    DF395.set(mask_position, true);
                }

            if ((sig == "5X") && (sys == "G"))
                {
                    mask_position = 32 - 24;
                    DF395.set(mask_position, true);
                }
            if ((sig == "1B") && (sys == "E"))
                {
                    mask_position = 32 - 4;
                    DF395.set(mask_position, true);
                }

            if ((sig == "5X") && (sys == "E"))
                {
                    mask_position = 32 - 24;
                    DF395.set(mask_position, true);
                }
            if ((sig == "7X") && (sys == "E"))
                {
                    mask_position = 32 - 16;
                    DF395.set(mask_position, true);
                }
            if ((sig == "1C") && (sys == "R"))
                {
                    mask_position = 32 - 2;
                    DF395.set(mask_position, true);
                }
            if ((sig == "2C") && (sys == "R"))
                {
                    mask_position = 32 - 8;
                    DF395.set(mask_position, true);
                }
        }

    return 0;
}


std::string Rtcm::set_DF396(const std::map<int32_t, Gnss_Synchro>& observables)
{
    std::string DF396;
    std::map<int32_t, Gnss_Synchro>::const_iterator observables_iter;
    Rtcm::set_DF394(observables);
    Rtcm::set_DF395(observables);
    uint32_t num_signals = DF395.count();
    uint32_t num_satellites = DF394.count();

    if ((num_signals == 0) || (num_satellites == 0))
        {
            std::string s("");
            return s;
        }
    std::vector<std::vector<bool>> matrix(num_signals, std::vector<bool>());

    std::string sig;
    std::vector<uint32_t> list_of_sats;
    std::vector<int> list_of_signals;

    for (observables_iter = observables.cbegin();
        observables_iter != observables.cend();
        observables_iter++)
        {
            list_of_sats.push_back(observables_iter->second.PRN);

            const std::string sig_(observables_iter->second.Signal);
            sig = sig_.substr(0, 2);

            const std::string sys(&observables_iter->second.System, 1);

            if ((sig == "1C") && (sys == "G"))
                {
                    list_of_signals.push_back(32 - 2);
                }
            if ((sig == "2S") && (sys == "G"))
                {
                    list_of_signals.push_back(32 - 15);
                }

            if ((sig == "5X") && (sys == "G"))
                {
                    list_of_signals.push_back(32 - 24);
                }
            if ((sig == "1B") && (sys == "E"))
                {
                    list_of_signals.push_back(32 - 4);
                }

            if ((sig == "5X") && (sys == "E"))
                {
                    list_of_signals.push_back(32 - 24);
                }
            if ((sig == "7X") && (sys == "E"))
                {
                    list_of_signals.push_back(32 - 16);
                }
        }

    std::sort(list_of_sats.begin(), list_of_sats.end());
    list_of_sats.erase(std::unique(list_of_sats.begin(), list_of_sats.end()), list_of_sats.end());

    std::sort(list_of_signals.begin(), list_of_signals.end());
    std::reverse(list_of_signals.begin(), list_of_signals.end());
    list_of_signals.erase(std::unique(list_of_signals.begin(), list_of_signals.end()), list_of_signals.end());

    // fill the matrix
    bool value;

    for (uint32_t row = 0; row < num_signals; row++)
        {
            for (uint32_t sat = 0; sat < num_satellites; sat++)
                {
                    value = false;
                    for (observables_iter = observables.cbegin();
                        observables_iter != observables.cend();
                        observables_iter++)
                        {
                            const std::string sig_(observables_iter->second.Signal);
                            sig = sig_.substr(0, 2);
                            const std::string sys(&observables_iter->second.System, 1);

                            if ((sig == "1C") && (sys == "G") && (list_of_signals.at(row) == 32 - 2) && (observables_iter->second.PRN == list_of_sats.at(sat)))
                                {
                                    value = true;
                                }

                            if ((sig == "2S") && (sys == "G") && (list_of_signals.at(row) == 32 - 15) && (observables_iter->second.PRN == list_of_sats.at(sat)))
                                {
                                    value = true;
                                }

                            if ((sig == "5X") && (sys == "G") && (list_of_signals.at(row) == 32 - 24) && (observables_iter->second.PRN == list_of_sats.at(sat)))
                                {
                                    value = true;
                                }

                            if ((sig == "1B") && (sys == "E") && (list_of_signals.at(row) == 32 - 4) && (observables_iter->second.PRN == list_of_sats.at(sat)))
                                {
                                    value = true;
                                }

                            if ((sig == "5X") && (sys == "E") && (list_of_signals.at(row) == 32 - 24) && (observables_iter->second.PRN == list_of_sats.at(sat)))
                                {
                                    value = true;
                                }

                            if ((sig == "7X") && (sys == "E") && (list_of_signals.at(row) == 32 - 16) && (observables_iter->second.PRN == list_of_sats.at(sat)))
                                {
                                    value = true;
                                }
                        }
                    matrix[row].push_back(value);
                }
        }

    // write the matrix column-wise
    DF396.clear();
    for (uint32_t col = 0; col < num_satellites; col++)
        {
            for (uint32_t row = 0; row < num_signals; row++)
                {
                    std::string ss;
                    if (matrix[row].at(col))
                        {
                            ss = "1";
                        }
                    else
                        {
                            ss = "0";
                        }
                    DF396 += ss;
                }
        }
    return DF396;
}


int32_t Rtcm::set_DF397(const Gnss_Synchro& gnss_synchro)
{
    const double meters_to_miliseconds = SPEED_OF_LIGHT_M_S * 0.001;
    const double rough_range_s = std::round(gnss_synchro.Pseudorange_m / meters_to_miliseconds / TWO_N10) * meters_to_miliseconds * TWO_N10;

    uint32_t int_ms = 0;

    if (rough_range_s == 0.0 || ((rough_range_s < 0.0) || (rough_range_s > meters_to_miliseconds * 255.0)))
        {
            int_ms = 255;
        }
    else
        {
            int_ms = static_cast<uint32_t>(std::lround(rough_range_s / meters_to_miliseconds / TWO_N10)) >> 10;
        }

    DF397 = std::bitset<8>(int_ms);
    return 0;
}


int32_t Rtcm::set_DF398(const Gnss_Synchro& gnss_synchro)
{
    const double meters_to_miliseconds = SPEED_OF_LIGHT_M_S * 0.001;
    const double rough_range_m = std::round(gnss_synchro.Pseudorange_m / meters_to_miliseconds / TWO_N10) * meters_to_miliseconds * TWO_N10;
    uint32_t rr_mod_ms;
    if ((rough_range_m <= 0.0) || (rough_range_m > meters_to_miliseconds * 255.0))
        {
            rr_mod_ms = 0;
        }
    else
        {
            rr_mod_ms = static_cast<uint32_t>(std::lround(rough_range_m / meters_to_miliseconds / TWO_N10)) & 0x3FFU;
        }
    DF398 = std::bitset<10>(rr_mod_ms);
    return 0;
}


int32_t Rtcm::set_DF399(const Gnss_Synchro& gnss_synchro)
{
    double lambda = 0.0;
    const std::string sig_(gnss_synchro.Signal);
    const std::string sig = sig_.substr(0, 2);

    if (sig == "1C")
        {
            lambda = SPEED_OF_LIGHT_M_S / GPS_L1_FREQ_HZ;
        }
    if (sig == "2S")
        {
            lambda = SPEED_OF_LIGHT_M_S / GPS_L2_FREQ_HZ;
        }
    if (sig == "5X")
        {
            lambda = SPEED_OF_LIGHT_M_S / GALILEO_E5A_FREQ_HZ;
        }
    if (sig == "1B")
        {
            lambda = SPEED_OF_LIGHT_M_S / GALILEO_E1_FREQ_HZ;
        }
    if (sig == "7X")
        {
            lambda = SPEED_OF_LIGHT_M_S / GALILEO_E5B_FREQ_HZ;
        }

    double rough_phase_range_rate_ms = std::round(-gnss_synchro.Carrier_Doppler_hz * lambda);
    if (rough_phase_range_rate_ms < -8191)
        {
            rough_phase_range_rate_ms = -8192;
        }
    if (rough_phase_range_rate_ms > 8191)
        {
            rough_phase_range_rate_ms = -8192;
        }

    DF399 = std::bitset<14>(static_cast<int32_t>(rough_phase_range_rate_ms));
    return 0;
}


int32_t Rtcm::set_DF400(const Gnss_Synchro& gnss_synchro)
{
    const double meters_to_miliseconds = SPEED_OF_LIGHT_M_S * 0.001;
    const double rough_range_m = std::round(gnss_synchro.Pseudorange_m / meters_to_miliseconds / TWO_N10) * meters_to_miliseconds * TWO_N10;
    const double psrng_s = gnss_synchro.Pseudorange_m - rough_range_m;
    int32_t fine_pseudorange;

    if (psrng_s == 0 || (std::fabs(psrng_s) > 292.7))
        {
            fine_pseudorange = -16384;  // 4000h: invalid value
        }
    else
        {
            fine_pseudorange = static_cast<int32_t>(std::round(psrng_s / meters_to_miliseconds / TWO_N24));
        }

    DF400 = std::bitset<15>(fine_pseudorange);
    return 0;
}


int32_t Rtcm::set_DF401(const Gnss_Synchro& gnss_synchro)
{
    const double meters_to_miliseconds = SPEED_OF_LIGHT_M_S * 0.001;
    const double rough_range_m = std::round(gnss_synchro.Pseudorange_m / meters_to_miliseconds / TWO_N10) * meters_to_miliseconds * TWO_N10;
    int64_t fine_phaserange;

    double lambda = 0.0;
    const std::string sig_(gnss_synchro.Signal);
    const std::string sig = sig_.substr(0, 2);
    const std::string sys(&gnss_synchro.System, 1);

    if ((sig == "1C") && (sys == "G"))
        {
            lambda = SPEED_OF_LIGHT_M_S / GPS_L1_FREQ_HZ;
        }
    else if ((sig == "2S") && (sys == "G"))
        {
            lambda = SPEED_OF_LIGHT_M_S / GPS_L2_FREQ_HZ;
        }
    else if ((sig == "5X") && (sys == "E"))
        {
            lambda = SPEED_OF_LIGHT_M_S / GALILEO_E5A_FREQ_HZ;
        }
    else if ((sig == "1B") && (sys == "E"))
        {
            lambda = SPEED_OF_LIGHT_M_S / GALILEO_E1_FREQ_HZ;
        }
    else if ((sig == "7X") && (sys == "E"))
        {
            lambda = SPEED_OF_LIGHT_M_S / GALILEO_E5B_FREQ_HZ;
        }
    else if ((sig == "1C") && (sys == "R"))
        {
            lambda = SPEED_OF_LIGHT_M_S / ((GLONASS_L1_CA_FREQ_HZ + (GLONASS_L1_CA_DFREQ_HZ * GLONASS_PRN.at(gnss_synchro.PRN))));
        }
    else if ((sig == "2C") && (sys == "R"))
        {
            // TODO Need to add slot number and freq number to gnss_syncro
            lambda = SPEED_OF_LIGHT_M_S / (GLONASS_L2_CA_FREQ_HZ);
        }
    else
        {
            // should not happen
            LOG(WARNING) << "Unknown signal in the generation of RTCM message DF401";
            lambda = SPEED_OF_LIGHT_M_S / GPS_L1_FREQ_HZ;
        }
    double phrng_m = (gnss_synchro.Carrier_phase_rads / TWO_PI) * lambda - rough_range_m;

    /* Subtract phase - pseudorange integer cycle offset */
    /* TODO: check LLI! */
    double cp = gnss_synchro.Carrier_phase_rads / TWO_PI;  // ?
    if (std::fabs(phrng_m - cp) > 1171.0)
        {
            cp = std::round(phrng_m / lambda) * lambda;
        }
    phrng_m -= cp;

    if (phrng_m == 0.0 || (std::fabs(phrng_m) > 1171.0))
        {
            fine_phaserange = -2097152;
        }
    else
        {
            fine_phaserange = static_cast<int64_t>(std::round(phrng_m / meters_to_miliseconds / TWO_N29));
        }

    DF401 = std::bitset<22>(fine_phaserange);
    return 0;
}


int32_t Rtcm::set_DF402(const Gps_Ephemeris& ephNAV, const Gps_CNAV_Ephemeris& ephCNAV, const Galileo_Ephemeris& ephFNAV, const Glonass_Gnav_Ephemeris& ephGNAV, double obs_time, const Gnss_Synchro& gnss_synchro)
{
    uint32_t lock_time_period_s = 0;
    const std::string sig_(gnss_synchro.Signal);
    const std::string sys(&gnss_synchro.System, 1);
    if ((sig_ == "1C") && (sys == "G"))
        {
            lock_time_period_s = Rtcm::lock_time(ephNAV, obs_time, gnss_synchro);
        }
    if ((sig_ == "2S") && (sys == "G"))
        {
            lock_time_period_s = Rtcm::lock_time(ephCNAV, obs_time, gnss_synchro);
        }
    // TODO Should add system for galileo satellites
    if ((sig_ == "1B") || (sig_ == "5X") || (sig_ == "7X") || (sig_ == "8X"))
        {
            lock_time_period_s = Rtcm::lock_time(ephFNAV, obs_time, gnss_synchro);
        }
    if (((sig_ == "1C") && (sys == "R")) || ((sig_ == "2C") && (sys == "R")))
        {
            lock_time_period_s = Rtcm::lock_time(ephGNAV, obs_time, gnss_synchro);
        }
    const uint32_t lock_time_indicator = Rtcm::msm_lock_time_indicator(lock_time_period_s);
    DF402 = std::bitset<4>(lock_time_indicator);
    return 0;
}


int32_t Rtcm::set_DF403(const Gnss_Synchro& gnss_synchro)
{
    const auto cnr_dB_Hz = static_cast<uint32_t>(std::round(gnss_synchro.CN0_dB_hz));
    DF403 = std::bitset<6>(cnr_dB_Hz);
    return 0;
}


int32_t Rtcm::set_DF404(const Gnss_Synchro& gnss_synchro)
{
    double lambda = 0.0;
    const std::string sig_(gnss_synchro.Signal);
    const std::string sig = sig_.substr(0, 2);
    int32_t fine_phaserange_rate;
    const std::string sys_(&gnss_synchro.System, 1);

    if ((sig_ == "1C") && (sys_ == "G"))
        {
            lambda = SPEED_OF_LIGHT_M_S / GPS_L1_FREQ_HZ;
        }
    if ((sig_ == "2S") && (sys_ == "G"))
        {
            lambda = SPEED_OF_LIGHT_M_S / GPS_L2_FREQ_HZ;
        }
    if ((sig_ == "5X") && (sys_ == "E"))
        {
            lambda = SPEED_OF_LIGHT_M_S / GALILEO_E5A_FREQ_HZ;
        }
    if ((sig_ == "1B") && (sys_ == "E"))
        {
            lambda = SPEED_OF_LIGHT_M_S / GALILEO_E1_FREQ_HZ;
        }
    if ((sig_ == "7X") && (sys_ == "E"))
        {
            lambda = SPEED_OF_LIGHT_M_S / GALILEO_E5B_FREQ_HZ;
        }
    if ((sig_ == "1C") && (sys_ == "R"))
        {
            lambda = SPEED_OF_LIGHT_M_S / (GLONASS_L1_CA_FREQ_HZ + (GLONASS_L1_CA_DFREQ_HZ * GLONASS_PRN.at(gnss_synchro.PRN)));
        }
    if ((sig_ == "2C") && (sys_ == "R"))
        {
            // TODO Need to add slot number and freq number to gnss syncro
            lambda = SPEED_OF_LIGHT_M_S / (GLONASS_L2_CA_FREQ_HZ);
        }
    const double rough_phase_range_rate = std::round(-gnss_synchro.Carrier_Doppler_hz * lambda);
    const double phrr = (-gnss_synchro.Carrier_Doppler_hz * lambda - rough_phase_range_rate);

    if (phrr == 0.0 || (std::fabs(phrr) > 1.6384))
        {
            fine_phaserange_rate = -16384;
        }
    else
        {
            fine_phaserange_rate = static_cast<int32_t>(std::round(phrr / 0.0001));
        }

    DF404 = std::bitset<15>(fine_phaserange_rate);
    return 0;
}


int32_t Rtcm::set_DF405(const Gnss_Synchro& gnss_synchro)
{
    const double meters_to_miliseconds = SPEED_OF_LIGHT_M_S * 0.001;
    const double rough_range_m = std::round(gnss_synchro.Pseudorange_m / meters_to_miliseconds / TWO_N10) * meters_to_miliseconds * TWO_N10;
    const double psrng_s = gnss_synchro.Pseudorange_m - rough_range_m;
    int64_t fine_pseudorange;

    if (psrng_s == 0.0 || (std::fabs(psrng_s) > 292.7))
        {
            fine_pseudorange = -524288;
        }
    else
        {
            fine_pseudorange = static_cast<int64_t>(std::round(psrng_s / meters_to_miliseconds / TWO_N29));
        }
    DF405 = std::bitset<20>(fine_pseudorange);
    return 0;
}


int32_t Rtcm::set_DF406(const Gnss_Synchro& gnss_synchro)
{
    int64_t fine_phaserange_ex;
    const double meters_to_miliseconds = SPEED_OF_LIGHT_M_S * 0.001;
    const double rough_range_m = std::round(gnss_synchro.Pseudorange_m / meters_to_miliseconds / TWO_N10) * meters_to_miliseconds * TWO_N10;
    double phrng_m;
    double lambda = 0.0;
    std::string sig_(gnss_synchro.Signal);
    sig_ = sig_.substr(0, 2);
    const std::string sys_(&gnss_synchro.System, 1);

    if ((sig_ == "1C") && (sys_ == "G"))
        {
            lambda = SPEED_OF_LIGHT_M_S / GPS_L1_FREQ_HZ;
        }
    else if ((sig_ == "2S") && (sys_ == "G"))
        {
            lambda = SPEED_OF_LIGHT_M_S / GPS_L2_FREQ_HZ;
        }
    else if ((sig_ == "5X") && (sys_ == "E"))
        {
            lambda = SPEED_OF_LIGHT_M_S / GALILEO_E5A_FREQ_HZ;
        }
    else if ((sig_ == "1B") && (sys_ == "E"))
        {
            lambda = SPEED_OF_LIGHT_M_S / GALILEO_E1_FREQ_HZ;
        }
    else if ((sig_ == "7X") && (sys_ == "E"))
        {
            lambda = SPEED_OF_LIGHT_M_S / GALILEO_E5B_FREQ_HZ;
        }
    else if ((sig_ == "1C") && (sys_ == "R"))
        {
            lambda = SPEED_OF_LIGHT_M_S / (GLONASS_L1_CA_FREQ_HZ + (GLONASS_L1_CA_DFREQ_HZ * GLONASS_PRN.at(gnss_synchro.PRN)));
        }
    else if ((sig_ == "2C") && (sys_ == "R"))
        {
            // TODO Need to add slot number and freq number to gnss syncro
            lambda = SPEED_OF_LIGHT_M_S / (GLONASS_L2_CA_FREQ_HZ);
        }
    else
        {
            // should not happen
            LOG(WARNING) << "Unknown signal in the generation of RTCM message DF406";
            lambda = SPEED_OF_LIGHT_M_S / GPS_L1_FREQ_HZ;
        }
    phrng_m = (gnss_synchro.Carrier_phase_rads / TWO_PI) * lambda - rough_range_m;

    /* Subtract phase - pseudorange integer cycle offset */
    /* TODO: check LLI! */
    double cp = gnss_synchro.Carrier_phase_rads / TWO_PI;  // ?
    if (std::fabs(phrng_m - cp) > 1171.0)
        {
            cp = std::round(phrng_m / lambda) * lambda;
        }
    phrng_m -= cp;

    if (phrng_m == 0.0 || (std::fabs(phrng_m) > 1171.0))
        {
            fine_phaserange_ex = -8388608;
        }
    else
        {
            fine_phaserange_ex = static_cast<int64_t>(std::round(phrng_m / meters_to_miliseconds / TWO_N31));
        }

    DF406 = std::bitset<24>(fine_phaserange_ex);
    return 0;
}


int32_t Rtcm::set_DF407(const Gps_Ephemeris& ephNAV, const Gps_CNAV_Ephemeris& ephCNAV, const Galileo_Ephemeris& ephFNAV, const Glonass_Gnav_Ephemeris& ephGNAV, double obs_time, const Gnss_Synchro& gnss_synchro)
{
    uint32_t lock_time_period_s = 0;

    const std::string sig_(gnss_synchro.Signal);
    const std::string sys_(&gnss_synchro.System, 1);
    if ((sig_ == "1C") && (sys_ == "G"))
        {
            lock_time_period_s = Rtcm::lock_time(ephNAV, obs_time, gnss_synchro);
        }
    if ((sig_ == "2S") && (sys_ == "G"))
        {
            lock_time_period_s = Rtcm::lock_time(ephCNAV, obs_time, gnss_synchro);
        }
    if (((sig_ == "1B") || (sig_ == "5X") || (sig_ == "7X") || (sig_ == "8X")) && (sys_ == "E"))
        {
            lock_time_period_s = Rtcm::lock_time(ephFNAV, obs_time, gnss_synchro);
        }
    if ((sig_ == "1C") && (sys_ == "R"))
        {
            lock_time_period_s = Rtcm::lock_time(ephGNAV, obs_time, gnss_synchro);
        }
    if ((sig_ == "2C") && (sys_ == "R"))
        {
            lock_time_period_s = Rtcm::lock_time(ephGNAV, obs_time, gnss_synchro);
        }
    const uint32_t lock_time_indicator = Rtcm::msm_extended_lock_time_indicator(lock_time_period_s);
    DF407 = std::bitset<10>(lock_time_indicator);
    return 0;
}


int32_t Rtcm::set_DF408(const Gnss_Synchro& gnss_synchro)
{
    const auto cnr_dB_Hz = static_cast<uint32_t>(std::round(gnss_synchro.CN0_dB_hz / 0.0625));
    DF408 = std::bitset<10>(cnr_dB_Hz);
    return 0;
}


int32_t Rtcm::set_DF409(uint32_t iods)
{
    DF409 = std::bitset<3>(iods);
    return 0;
}


int32_t Rtcm::set_DF411(uint32_t clock_steering_indicator)
{
    DF411 = std::bitset<2>(clock_steering_indicator);
    return 0;
}


int32_t Rtcm::set_DF412(uint32_t external_clock_indicator)
{
    DF412 = std::bitset<2>(external_clock_indicator);
    return 0;
}


int32_t Rtcm::set_DF417(bool using_divergence_free_smoothing)
{
    DF417 = std::bitset<1>(using_divergence_free_smoothing);
    return 0;
}


int32_t Rtcm::set_DF418(int32_t carrier_smoothing_interval_s)
{
    if (carrier_smoothing_interval_s < 0)
        {
            DF418 = std::bitset<3>("111");
        }
    else
        {
            if (carrier_smoothing_interval_s == 0)
                {
                    DF418 = std::bitset<3>("000");
                }
            else if (carrier_smoothing_interval_s < 30)
                {
                    DF418 = std::bitset<3>("001");
                }
            else if (carrier_smoothing_interval_s < 60)
                {
                    DF418 = std::bitset<3>("010");
                }
            else if (carrier_smoothing_interval_s < 120)
                {
                    DF418 = std::bitset<3>("011");
                }
            else if (carrier_smoothing_interval_s < 240)
                {
                    DF418 = std::bitset<3>("100");
                }
            else if (carrier_smoothing_interval_s < 480)
                {
                    DF418 = std::bitset<3>("101");
                }
            else
                {
                    DF418 = std::bitset<3>("110");
                }
        }
    return 0;
}


int32_t Rtcm::set_DF420(const Gnss_Synchro& gnss_synchro __attribute__((unused)))
{
    // todo: read the value from gnss_synchro
    bool half_cycle_ambiguity_indicator = false;
    DF420 = std::bitset<1>(half_cycle_ambiguity_indicator);
    return 0;
}


// IGS State Space Representation (SSR) data fields
// see https://files.igs.org/pub/data/format/igs_ssr_v1.pdf

void Rtcm::set_IDF001(uint8_t version)
{
    const uint8_t max_value = 7;
    uint8_t igm_version = version;
    if (igm_version > max_value)  // not defined
        {
            LOG(WARNING) << "RTCM SSR messages are probably wrong: bad IGM/IM Version";
            igm_version = 0;
        }
    IDF001 = std::bitset<3>(igm_version);
}


void Rtcm::set_IDF002(uint8_t igs_message_number)
{
    IDF002 = std::bitset<8>(igs_message_number);
}


void Rtcm::set_IDF003(uint32_t tow)
{
    const uint32_t max_value = 604799;
    uint32_t ssr_epoch_time = tow;
    if (ssr_epoch_time > max_value)
        {
            LOG(WARNING) << "RTCM SSR messages are probably wrong: bad SSR Epoch Time";
            ssr_epoch_time = 0;
        }
    IDF003 = std::bitset<20>(ssr_epoch_time);
}


void Rtcm::set_IDF004(uint8_t ssr_update_interval)
{
    const uint8_t max_value = 15;
    uint8_t update_interval = ssr_update_interval;
    if (update_interval > max_value)
        {
            LOG(WARNING) << "RTCM SSR messages are probably wrong: bad SSR Update Interval";
            update_interval = 0;
        }
    IDF004 = std::bitset<4>(update_interval);
}


void Rtcm::set_IDF005(bool ssr_multiple_message_indicator)
{
    IDF005 = std::bitset<1>(ssr_multiple_message_indicator);
}


void Rtcm::set_IDF006(bool regional_indicator)
{
    IDF006 = std::bitset<1>(regional_indicator);
}


void Rtcm::set_IDF007(uint8_t ssr_iod)
{
    const uint8_t max_value = 15;
    uint8_t iod = ssr_iod;
    if (iod > max_value)
        {
            LOG(WARNING) << "RTCM SSR messages are probably wrong: bad IOD SSR";
            iod = 0;
        }
    IDF007 = std::bitset<4>(iod);
}


void Rtcm::set_IDF008(uint16_t ssr_provider_id)
{
    IDF008 = std::bitset<16>(ssr_provider_id);
}


void Rtcm::set_IDF009(uint8_t ssr_solution_id)
{
    const uint8_t max_value = 15;
    uint8_t sol_id = ssr_solution_id;
    if (sol_id > max_value)
        {
            LOG(WARNING) << "RTCM SSR messages are probably wrong: bad SSR Solution ID";
            sol_id = 0;
        }
    IDF009 = std::bitset<4>(sol_id);
}


void Rtcm::set_IDF010(uint8_t num_satellites)
{
    const uint8_t max_value = 63;
    uint8_t num_sats = num_satellites;
    if (num_sats > max_value)
        {
            LOG(WARNING) << "RTCM SSR messages are probably wrong: bad number of satellites";
            num_sats = 0;
        }
    IDF010 = std::bitset<6>(num_sats);
}


void Rtcm::set_IDF011(uint8_t gnss_satellite_id)
{
    const uint8_t max_value = 63;
    uint8_t sat_id = gnss_satellite_id;
    if (sat_id > max_value)
        {
            LOG(WARNING) << "RTCM SSR messages are probably wrong: bad GNSS Satellite ID";
            sat_id = 0;
        }
    IDF011 = std::bitset<6>(sat_id);
}


void Rtcm::set_IDF012(uint8_t gnss_iod)
{
    IDF012 = std::bitset<8>(gnss_iod);
}


void Rtcm::set_IDF013(float delta_orbit_radial_m)
{
    const float scale = 1.0e4;
    const int32_t max_value = 2097151;
    auto delta_orbit_radial = static_cast<int32_t>((delta_orbit_radial_m * scale));
    if (delta_orbit_radial > max_value)
        {
            delta_orbit_radial = max_value;
        }
    if (delta_orbit_radial < -max_value)
        {
            delta_orbit_radial = -max_value;
        }
    IDF013 = std::bitset<22>(delta_orbit_radial);
}


void Rtcm::set_IDF014(float delta_orbit_in_track_m)
{
    const float scale = 2500.0;
    const int32_t max_value = 524287;
    auto delta_orbit_in_track = static_cast<int32_t>((delta_orbit_in_track_m * scale));
    if (delta_orbit_in_track > max_value)
        {
            delta_orbit_in_track = max_value;
        }
    if (delta_orbit_in_track < -max_value)
        {
            delta_orbit_in_track = -max_value;
        }
    IDF014 = std::bitset<20>(delta_orbit_in_track);
}


void Rtcm::set_IDF015(float delta_orbit_cross_track_m)
{
    const float scale = 2500.0;
    const int32_t max_value = 524287;
    auto delta_orbit_cross_track = static_cast<int32_t>((delta_orbit_cross_track_m * scale));
    if (delta_orbit_cross_track > max_value)
        {
            delta_orbit_cross_track = max_value;
        }
    if (delta_orbit_cross_track < -max_value)
        {
            delta_orbit_cross_track = -max_value;
        }
    IDF015 = std::bitset<20>(delta_orbit_cross_track);
}


void Rtcm::set_IDF016(float dot_orbit_delta_track_m_s)
{
    const float scale = 1.0e6;
    const int32_t max_value = 1048575;
    auto dot_orbit_delta_track = static_cast<int32_t>((dot_orbit_delta_track_m_s * scale));
    if (dot_orbit_delta_track > max_value)
        {
            dot_orbit_delta_track = max_value;
        }
    if (dot_orbit_delta_track < -max_value)
        {
            dot_orbit_delta_track = -max_value;
        }
    IDF016 = std::bitset<21>(dot_orbit_delta_track);
}


void Rtcm::set_IDF017(float dot_orbit_delta_in_track_m_s)
{
    const float scale = 250000.0;
    const int32_t max_value = 262143;
    auto dot_orbit_delta_in_track = static_cast<int32_t>((dot_orbit_delta_in_track_m_s * scale));
    if (dot_orbit_delta_in_track > max_value)
        {
            dot_orbit_delta_in_track = max_value;
        }
    if (dot_orbit_delta_in_track < -max_value)
        {
            dot_orbit_delta_in_track = -max_value;
        }
    IDF017 = std::bitset<19>(dot_orbit_delta_in_track);
}


void Rtcm::set_IDF018(float dot_orbit_delta_cross_track_m_s)
{
    const float scale = 250000.0;
    const int32_t max_value = 262143;
    auto dot_orbit_delta_cross_track = static_cast<int32_t>((dot_orbit_delta_cross_track_m_s * scale));
    if (dot_orbit_delta_cross_track > max_value)
        {
            dot_orbit_delta_cross_track = max_value;
        }
    if (dot_orbit_delta_cross_track < -max_value)
        {
            dot_orbit_delta_cross_track = -max_value;
        }
    IDF018 = std::bitset<19>(dot_orbit_delta_cross_track);
}


void Rtcm::set_IDF019(float delta_clock_c0_m)
{
    const float scale = 1.0e4;
    const int32_t max_value = 2097151;
    auto delta_clock_c0 = static_cast<int32_t>((delta_clock_c0_m * scale));
    if (delta_clock_c0 > max_value)
        {
            delta_clock_c0 = max_value;
        }
    if (delta_clock_c0 < -max_value)
        {
            delta_clock_c0 = -max_value;
        }
    IDF019 = std::bitset<22>(delta_clock_c0);
}


void Rtcm::set_IDF020(float delta_clock_c1_m_s)
{
    const float scale = 1.0e6;
    const int32_t max_value = 1048575;
    auto delta_clock_c1 = static_cast<int32_t>((delta_clock_c1_m_s * scale));
    if (delta_clock_c1 > max_value)
        {
            delta_clock_c1 = max_value;
        }
    if (delta_clock_c1 < -max_value)
        {
            delta_clock_c1 = -max_value;
        }
    IDF020 = std::bitset<21>(delta_clock_c1);
}


void Rtcm::set_IDF021(float delta_clock_c2_m_s2)
{
    const float scale = 5.0e8;
    const int32_t max_value = 67108863;
    auto delta_clock_c2 = static_cast<int32_t>((delta_clock_c2_m_s2 * scale));
    if (delta_clock_c2 > max_value)
        {
            delta_clock_c2 = max_value;
        }
    if (delta_clock_c2 < -max_value)
        {
            delta_clock_c2 = -max_value;
        }
    IDF021 = std::bitset<27>(delta_clock_c2);
}


void Rtcm::set_IDF023(uint8_t num_bias_processed)
{
    const uint8_t max_value = 31;
    uint8_t num_bias = num_bias_processed;
    if (num_bias > max_value)
        {
            LOG(WARNING) << "RTCM SSR messages are probably wrong: bad number of biases processed";
            num_bias = 0;
        }
    IDF023 = std::bitset<5>(num_bias);
}


void Rtcm::set_IDF024(uint8_t gnss_signal_tracking_mode_id)
{
    const uint8_t max_value = 31;
    uint8_t tracking_mode = gnss_signal_tracking_mode_id;
    if (tracking_mode > max_value)  // error
        {
            LOG(WARNING) << "RTCM SSR messages are probably wrong: bad GNSS Signal and Tracking Mode Identifier";
            tracking_mode = 0;
        }
    IDF024 = std::bitset<5>(tracking_mode);
}


void Rtcm::set_IDF025(float code_bias_m)
{
    const float scale = 100.0;
    const int16_t max_value = 8191;
    auto code_bias = static_cast<int16_t>((code_bias_m * scale));
    if (code_bias > max_value)
        {
            code_bias = max_value;
        }
    if (code_bias < -max_value)
        {
            code_bias = -max_value;
        }
    IDF025 = std::bitset<14>(code_bias);
}
