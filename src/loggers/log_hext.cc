//--------------------------------------------------------------------------
// Copyright (C) 2015-2015 Cisco and/or its affiliates. All rights reserved.
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

// log_hext.cc author Russ Combs <rucombs@cisco.com>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <ctype.h>
#include <string.h>

#include <algorithm>
#include <iostream>
using namespace std;

#include "main/snort_types.h"
#include "framework/logger.h"
#include "framework/module.h"
#include "protocols/packet.h"
#include "log/text_log.h"
#include "sfip/sf_ip.h"

#define S_NAME "log_hext"
#define F_NAME S_NAME ".txt"

static const char* s_help = "output payload suitable for daq hext";

static THREAD_LOCAL TextLog* hext_log = nullptr;
static THREAD_LOCAL unsigned s_pkt_num = 0;

//-------------------------------------------------------------------------
// impl stuff
//-------------------------------------------------------------------------

#define LOG_CHARS 20

static void log_raw(const Packet* p)
{
    TextLog_Print(hext_log, "\n# %u [%u]\n",
        s_pkt_num++, p->pkth->caplen);
}

static void log_header(const Packet* p)
{
    char src[INET6_ADDRSTRLEN];
    char dst[INET6_ADDRSTRLEN];

    const sfip_t* addr = p->ptrs.ip_api.get_src();
    sfip_ntop(addr, src, sizeof(src));

    addr = p->ptrs.ip_api.get_dst();
    sfip_ntop(addr, dst, sizeof(dst));

    TextLog_Print(hext_log, "\n$packet %s %d -> %s %d\n",
        src, p->ptrs.sp, dst, p->ptrs.dp);
}

static void log_data(const uint8_t* p, unsigned n)
{
    char hex[(3*LOG_CHARS)+1];
    char txt[LOG_CHARS+1];
    unsigned odx = 0, idx = 0;

    TextLog_NewLine(hext_log);

    for ( idx = 0; idx < n; idx++)
    {
        uint8_t byte = p[idx];
        sprintf(hex + 3*odx, "%2.02X ", byte);
        txt[odx++] = isprint(byte) ? byte : '.';

        if ( odx == LOG_CHARS )
        {
            txt[odx] = hex[3*odx] = '\0';
            TextLog_Print(hext_log, "x%s # %s\n", hex, txt);
            odx = 0;
        }
    }
    if ( odx )
    {
        txt[odx] = hex[3*odx] = '\0';
        TextLog_Print(hext_log, "x%s", hex);

        while ( odx++ < LOG_CHARS )
            TextLog_Print(hext_log, "   ");

        TextLog_Print(hext_log, " # %s\n", txt);
    }
}

//-------------------------------------------------------------------------
// module stuff
//-------------------------------------------------------------------------

static const Parameter s_params[] =
{
    { "file", Parameter::PT_BOOL, nullptr, "false",
      "output to " F_NAME " instead of stdout" },

    { "raw", Parameter::PT_BOOL, nullptr, "false",
      "output all full packets if true, else just TCP payload" },

    { "limit", Parameter::PT_INT, "0:", "0",
      "set limit (0 is unlimited)" },

    { "units", Parameter::PT_ENUM, "B | K | M | G", "B",
      "bytes | KB | MB | GB" },

    { nullptr, Parameter::PT_MAX, nullptr, nullptr, nullptr }
};

class HextModule : public Module
{
public:
    HextModule() : Module(S_NAME, s_help, s_params) { }

    bool set(const char*, Value&, SnortConfig*) override;
    bool begin(const char*, int, SnortConfig*) override;
    bool end(const char*, int, SnortConfig*) override;

public:
    bool file;
    bool raw;
    unsigned long limit;
    unsigned units;
};

bool HextModule::set(const char*, Value& v, SnortConfig*)
{
    if ( v.is("file") )
        file = v.get_bool();

    else if ( v.is("raw") )
        raw = v.get_bool();

    else if ( v.is("limit") )
        limit = v.get_long();

    else if ( v.is("units") )
        units = v.get_long();

    else
        return false;

    return true;
}

bool HextModule::begin(const char*, int, SnortConfig*)
{
    file = false;
    raw = false;
    limit = 0;
    units = 0;
    return true;
}

bool HextModule::end(const char*, int, SnortConfig*)
{
    while ( units-- )
        limit *= 1024;

    return true;
}

//-------------------------------------------------------------------------
// logger stuff
//-------------------------------------------------------------------------

class HextLogger : public Logger
{
public:
    HextLogger(HextModule*);

    void open() override;
    void close() override;

    void log(Packet*, const char* msg, Event*) override;

private:
    string file;
    unsigned long limit;
    bool raw;
};

HextLogger::HextLogger(HextModule* m)
{
    file = m->file ? F_NAME : "stdout";
    limit = m->limit;
    raw = m->raw;
}

void HextLogger::open()
{
    const unsigned buf_sz = 65536;
    hext_log = TextLog_Init(file.c_str(), buf_sz, limit);
}

void HextLogger::close()
{
    if ( hext_log )
        TextLog_Term(hext_log);
}

void HextLogger::log(Packet* p, const char*, Event*)
{
    if ( raw )
    {
        log_raw(p);
        log_data(p->pkt, p->pkth->caplen);
    }
    else if ( p->has_tcp_data() )
    {
        log_header(p);
        log_data(p->data, p->dsize);
    }
}

//-------------------------------------------------------------------------
// api stuff
//-------------------------------------------------------------------------

static Module* mod_ctor()
{ return new HextModule; }

static void mod_dtor(Module* m)
{ delete m; }

static Logger* hext_ctor(SnortConfig*, Module* mod)
{
    return new HextLogger((HextModule*)mod);
}

static void hext_dtor(Logger* p)
{ delete p; }

static const LogApi hext_api =
{
    {
        PT_LOGGER,
        sizeof(LogApi),
        LOGAPI_VERSION,
        0,
        API_RESERVED,
        API_OPTIONS,
        S_NAME,
        s_help,
        mod_ctor,
        mod_dtor
    },
    OUTPUT_TYPE_FLAG__ALERT,
    hext_ctor,
    hext_dtor
};

SO_PUBLIC const BaseApi* snort_plugins[] =
{
    &hext_api.base,
    nullptr
};

