//--------------------------------------------------------------------------
// Copyright (C) 2018-2019 Cisco and/or its affiliates. All rights reserved.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 2 as published
// by the Free Software Foundation.  You may not use, modify or distribute
// this program under any other version of the GNU General Public License.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//--------------------------------------------------------------------------
// http2_flow_data.cc author Tom Peters <thopeter@cisco.com>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "http2_flow_data.h"

#include "service_inspectors/http_inspect/http_test_manager.h"

#include "http2_enum.h"
#include "http2_module.h"
#include "http2_start_line.h"

using namespace snort;
using namespace Http2Enums;
using namespace HttpCommon;

unsigned Http2FlowData::inspector_id = 0;

#ifdef REG_TEST
uint64_t Http2FlowData::instance_count = 0;
#endif

Http2FlowData::Http2FlowData() : FlowData(inspector_id)
{
#ifdef REG_TEST
    seq_num = ++instance_count;
    if (HttpTestManager::use_test_output(HttpTestManager::IN_HTTP2) &&
        !HttpTestManager::use_test_input(HttpTestManager::IN_HTTP2))
    {
        printf("HTTP/2 Flow Data construct %" PRIu64 "\n", seq_num);
        fflush(nullptr);
    }
#endif
    Http2Module::increment_peg_counts(PEG_CONCURRENT_SESSIONS);
    if (Http2Module::get_peg_counts(PEG_MAX_CONCURRENT_SESSIONS) <
        Http2Module::get_peg_counts(PEG_CONCURRENT_SESSIONS))
        Http2Module::increment_peg_counts(PEG_MAX_CONCURRENT_SESSIONS);
}

Http2FlowData::~Http2FlowData()
{
#ifdef REG_TEST
    if (HttpTestManager::use_test_output(HttpTestManager::IN_HTTP2) &&
        !HttpTestManager::use_test_input(HttpTestManager::IN_HTTP2))
    {
        printf("HTTP/2 Flow Data destruct %" PRIu64 "\n", seq_num);
        fflush(nullptr);
    }
#endif
    if (Http2Module::get_peg_counts(PEG_CONCURRENT_SESSIONS) > 0)
        Http2Module::decrement_peg_counts(PEG_CONCURRENT_SESSIONS);

    for (int k=0; k <= 1; k++)
    {
        delete[] frame_header[k];
        delete[] frame_data[k];
        delete[] raw_decoded_header[k];
        delete infractions[k];
        delete events[k];
        delete http2_decoded_header[k];
    }
}

void Http2FlowData::clear_frame_data(HttpCommon::SourceId source_id)
{
    // If there is more data to be inspected in the frame, leave the frame_header
    if (leftover_data[source_id] == 0)
    {
        delete[] frame_header[source_id];
        frame_header[source_id] = nullptr;
    }
    delete[] frame_data[source_id];
    frame_data[source_id] = nullptr;
    frame_in_detection = false;
    delete[] raw_decoded_header[source_id];
    raw_decoded_header[source_id] = nullptr;
    continuation_expected[source_id] = false;
    frames_aggregated[source_id] = 0;
    scan_header_octets_seen[source_id] = 0;
    delete header_start_line[source_id];
    header_start_line[source_id] = nullptr;
    delete http2_decoded_header[source_id];
    http2_decoded_header[source_id] = nullptr;
}
