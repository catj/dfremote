//
//  remote.cpp
//  remote
//
//  Created by Vitaly Pronkin on 14/05/14.
//  Copyright (c) 2014 mifki. All rights reserved.
//

#include <sys/stat.h>
#include <stdint.h>
#include <math.h>
#include <iostream>
#include <map>
#include <vector>
#include <queue>
#include <zlib.h>

#if defined(WIN32)
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>

    float roundf(float x)
    {
       return x >= 0.0f ? floorf(x + 0.5f) : ceilf(x - 0.5f);
    }

#elif defined(__APPLE__)

#else
#endif

#include "Core.h"
#include "Console.h"
#include "Export.h"
#include "PluginManager.h"
#include "VTableInterpose.h"
#include "MemAccess.h"
#include "VersionInfo.h"
#include "TileTypes.h"
#include "LuaTools.h"
#include "modules/Maps.h"
#include "modules/World.h"
#include "modules/MapCache.h"
#include "modules/Gui.h"
#include "modules/Screen.h"
#include "modules/Buildings.h"
#include "modules/Units.h"
#include "modules/Items.h"
#include "df/graphic.h"
#include "df/enabler.h"
#include "df/renderer.h"
#include "df/interfacest.h"
#include "df/viewscreen_dwarfmodest.h"
#include "df/viewscreen_topicmeeting_takerequestsst.h"
#include "df/viewscreen_topicmeetingst.h"
#include "df/viewscreen_meetingst.h"

#include "tinythread.h"

#include <enet/enet.h>

#include "sha256.h"

using df::global::world;
using std::string;
using std::vector;
using df::global::enabler;
using df::global::gps;
using df::global::ui;
using df::global::init;
using df::global::d_init;
using df::global::gview;

static bool enabled;
static color_ostream *out2;

static int gwindow_x, gwindow_y, gwindow_z;

static int gmenu_w;

// Buffers for map rendering
static uint8_t *_gscreen[2];
static int32_t *_gscreentexpos[2];
static int8_t *_gscreentexpos_addcolor[2];
static uint8_t *_gscreentexpos_grayscale[2];
static uint8_t *_gscreentexpos_cf[2];
static uint8_t *_gscreentexpos_cbr[2];

// Current buffers
static uint8_t *gscreen;
static int32_t *gscreentexpos;
static int8_t *gscreentexpos_addcolor;
static uint8_t *gscreentexpos_grayscale, *gscreentexpos_cf, *gscreentexpos_cbr;

// Previous buffers to determine changed tiles
static uint8_t *gscreen_old;
static int32_t *gscreentexpos_old;
static int8_t *gscreentexpos_addcolor_old;
static uint8_t *gscreentexpos_grayscale_old, *gscreentexpos_cf_old, *gscreentexpos_cbr_old;

#include "patches.hpp"

#ifdef WIN32
    // On Windows there's no parameter pointing to the map_renderer structure
    typedef void (_stdcall *RENDER_MAP)(int);
#else
    typedef void (*RENDER_MAP)(void*, int);
#endif

RENDER_MAP _render_map;

#ifdef WIN32
    #define render_map() _render_map(0)
#else
    #define render_map() _render_map(df::global::map_renderer, 0)
#endif

static tthread::thread * enthread;
static void enthreadmain(ENetHost*);

unsigned char buf[65536];

struct rendered_block {
    unsigned char x, y, z;
    unsigned int data[16*16];
};

rendered_block *sent_blocks_idx[16][16][200];

static lua_State *L;

static int wx, wy;
static bool rendering;
static int newwidth, newheight;
static volatile bool waiting_render;
static volatile bool map_render_enabled;
static volatile bool render_initial;
static bool force_render_ui;

static unsigned int server_token;
static unsigned int world_token;
static bool remote_on;
static int enet_port = 1235;
static bool debug;

static string publish_name;
static long last_publish_attempt;
static bool mediation_connected;

static std::string pwd_hash;

void remote_publish(string &name);

std::list<ENetPacket*> outgoing;

static ENetPeer *client_peer = NULL;
static ENetPeer *mediation_peer = NULL;
static ENetHost *server;

std::string client_addr;

static int timer_timeout = -1;
static string timer_fn;

#include "dwarfmode.hpp"
#include "itemcache.hpp"
#include "corehacks.hpp"

std::string hash_password(std::string &pwd)
{
    std::string q = pwd;

    for (int i = 0; i < 517; i++)
        q = sha256(q);

    return q;
}

void resend_outgoing()
{
    for (auto it = outgoing.begin(); it != outgoing.end(); it++)
    {
        ENetPacket *pkt = *it;
        enet_peer_send (client_peer, 0, pkt);
    }
}


void packet_free_cb(ENetPacket *packet)
{
    //XXX: if several packets are sent at once, this function may be called not in the sending order
    //XXX: so we can't just use the first packet in the queue

    if (packet->flags & ENET_PACKET_FLAG_SENT)
        outgoing.remove(packet);
    else
    {
        ENetPacket * pkt = enet_packet_create (packet->data, packet->dataLength, packet->flags);
        pkt->freeCallback = packet_free_cb;

        std::list<ENetPacket*>::iterator findIter = std::find(outgoing.begin(), outgoing.end(), packet);
        if (findIter != outgoing.end())
            *findIter = pkt;
    }
}

unsigned char *zbuf = NULL;
int zbufsz = 0;

void send_enet(const unsigned char *buf, int sz, ENetPeer *peer)
{
    z_stream strm;
    bool compressed = false;

    if (sz >= 500)
    {
        strm.zalloc = Z_NULL;
        strm.zfree = Z_NULL;
        strm.opaque = Z_NULL;
        deflateInit(&strm, Z_DEFAULT_COMPRESSION);

        int zsz = deflateBound(&strm, sz);
        if (zsz > zbufsz)
        {
            free(zbuf);
            zbuf = (unsigned char*) malloc(zsz + 1);
            zbuf[0] = 129;
            zbufsz = zsz;
        }

        strm.avail_in = sz;
        strm.next_in = (Bytef*)buf;

        strm.avail_out = zsz;
        strm.next_out = zbuf + 1;
        int ret = deflate(&strm, Z_FINISH);    /* no bad return value */

        zsz = strm.next_out - zbuf;
        if (ret == Z_STREAM_END && zsz < sz)
        {
            buf = zbuf;
            sz = zsz + 1;
            compressed = true;
        }
        else
            deflateEnd(&strm);
    }

    ENetPacket * packet = enet_packet_create (buf, sz, ENET_PACKET_FLAG_RELIABLE);
    if (compressed)
        deflateEnd(&strm);        

    if (peer == client_peer)
    {
        packet->freeCallback = packet_free_cb;
        outgoing.push_back(packet);
    }

    enet_peer_send(peer, 0, packet);
}

std::string address2ip(ENetAddress *addr)
{
    char name[32];
    enet_address_get_host_ip(addr, name, sizeof(name));

    return name;    
}

typedef void(*send_func)(const unsigned char *buf, int sz, void *conn);


static int old_status1 = 0, old_status2 = 0;
static bool force_send_status;

void empty_map_cache()
{
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++)
            for (int z = 0; z < 200; z++)
                free(sent_blocks_idx[i][j][z]);

    memset(sent_blocks_idx, 0, sizeof(sent_blocks_idx));
}

bool block_is_unmined(int bx, int by, int zlevel)
{
    bool unmined = true;
    df::map_block *block = world->map.block_index[bx][by][zlevel];
    if (!block)
        return true;

    for (int j = 0; j < 16; j++)
        for (int i = 0; i < 16; i++)
            if (!block->designation[i][j].bits.hidden)
                return false;

    return true;
}

void send_initial_map(unsigned short seq, unsigned char startblk, send_func sendfunc, void *conn)
{
    int map_w = world->map.x_count;
    int map_h = world->map.y_count;

    int blk_w = map_w / 16;
    int blk_h = map_h / 16;

    if (!startblk)
    {
        int ox = wx;
        int oy = wy;
        int ow = newwidth;
        int oh = newheight;

        wx = 0;
        wy = 0;

        df::viewscreen *ws = Gui::getCurViewscreen();
        if (df::viewscreen_dwarfmodest::_identity.is_direct_instance(ws))
        {
            {
                CoreSuspender suspend;
                newwidth = map_w;
                newheight = map_h;
                waiting_render = true;
                render_initial = true;
            }

            while(waiting_render);
        }
        /**out2 << "aa" << std::endl;
        {
            CoreSuspender suspend;
            //while (enabler->outstanding_gframes);

            newwidth = map_w;
            newheight = map_h;
            render_remote_map();
            gps->force_full_display_count = 2;
        }*/
        /*{
            newwidth = map_w;
            newheight = map_h;
            core_force_render();
        }*/

        wx = ox;
        wy = oy;
        newwidth = ow;
        newheight = oh;
    }

    int zlevel = gwindow_z;

    unsigned char *b = buf;
    *(b++) = 128;

    *(unsigned short*)b = seq + 1;
    b += 2;

    *(b++) = zlevel;

    unsigned char *nextblk = b;
    *(b++) = 0;

    int cnt = 0;

    //XXX: for example, if the game has already ended, we're on end announcement screen
    if (!world->map.block_index)
    {
        goto enough;
    }

    for (int by = 0; by < blk_h; by++)
    {
        for (int bx = 0; bx < blk_w; bx++)
        {
            int idx = bx + by * blk_w;
            if (idx < startblk)
                continue;

            // Skip unmined or non-existent blocks
            if (block_is_unmined(bx, by, zlevel))
                continue;

            // Limit to number of blocks per msg
            if (++cnt >= 10)
            {
                *nextblk = idx;
                goto enough;
            }            

            rendered_block *rblk = sent_blocks_idx[bx][by][zlevel];
            if (!rblk)
                rblk = sent_blocks_idx[bx][by][zlevel] = (rendered_block*) calloc(1, sizeof(rendered_block));

            *(b++) = bx;
            *(b++) = by;

            for (int j = 0; j < 16; j++)
            {
                for (int i = 0; i < 16; i++)
                {
                    int x = bx*16 + i;
                    int y = by*16 + j;

                    const int tile = x * map_h + y;
                    unsigned char *s = gscreen + tile*4;
                    unsigned int *is = (unsigned int*)gscreen + tile;

                    *(b++) = s[0]; //ch

                    int bg = s[2];
                    int bold = (s[3] != 0) * 8;
                    int fg   = (s[1] + bold) % 16;

                    *(b++) = fg + (bg << 4);

                    rblk->data[i + j * 16] = *is;
                }
            }
        }
    }

    enough:;
    sendfunc(buf, (int)(b-buf), conn);

    if (!*nextblk)
        map_render_enabled = true;
}

bool send_map_updates(send_func sendfunc, void *conn)
{
    bool needs_sync = force_send_status;
    force_send_status = false;

    unsigned char *b = buf;

    unsigned char *firstb = buf;
    *(b++) = 0;

    {
        long t1 = enet_time_get();
        FastCoreSuspender suspend;
        long t2 = enet_time_get();
        //*out2 << (t2-t1) << std::endl;

        Lua::PushModulePublic(*out2, L, "remote", "get_status_ext");
        lua_pushinteger(L, needs_sync);
        if (Lua::SafeCall(*out2, L, 1, 4, true))
        {
            int status1 = lua_tointeger(L, -4);
            int status2 = lua_tointeger(L, -3);

            size_t len, len2;
            const unsigned char *extdata = (const unsigned char*) lua_tolstring(L, -2, &len);
            const unsigned char *centerdata = (const unsigned char*) lua_tolstring(L, -1, &len2);

            // Status
            if (old_status1 != status1 || old_status2 != status2 || needs_sync)
            {
                *firstb |= (1 << 0);

                *(b++) = status1;
                *(b++) = status2;

                old_status1 = status1;
                old_status2 = status2;
            }

            // Extended data
            if (extdata)
            {
                *firstb |= (1 << 1);
                memcpy(b, extdata, len);
                b += len;
            }

            // Center data
            if (centerdata)
            {
                int newwx = std::max(0, (int)roundf(centerdata[0] - newwidth/2.0f));
                int newwy = std::max(0, (int)roundf(centerdata[1] - newheight/2.0f));

                if (wx != newwx || wy != newwy || *df::global::window_z != centerdata[2])
                {
                    *firstb |= (1 << 2);
                    memcpy(b, centerdata, len2);
                    b += len2;

                    wx = newwx;
                    wy = newwy;
                    *df::global::window_z = centerdata[2];
                }
            }
        }
    }

    unsigned char *emptyb = b;

    static bool send_z = false;
    static int zlevel = 0;
    if (zlevel != gwindow_z)
    {
        send_z = true;
        zlevel = gwindow_z;
    }

    //TODO: shouldn't just skip if waiting_render is true, or at least don't update t2 if didn't send map here
    if (!waiting_render)
    {
        b++;
        //int tile = 0;
        int maxx = std::min(newwidth, world->map.x_count - gwindow_x);
        int maxy = std::min(newheight, world->map.y_count - gwindow_y);
        int cnt = 0;
        for (int y = 0; y < maxy; y++)
        {
            for (int x = 0; x < maxx; x++)
            {
                const int tile = x * newheight + y;
                unsigned char *s = gscreen + tile*4;

                int xx = x + gwindow_x;
                int yy = y + gwindow_y;
                rendered_block *rblk = sent_blocks_idx[xx>>4][yy>>4][zlevel];
                if (!rblk)
                    rblk = sent_blocks_idx[xx>>4][yy>>4][zlevel] = (rendered_block*) calloc(1, sizeof(rendered_block));

                unsigned int *is = (unsigned int*)gscreen + tile;
                if (*is != rblk->data[xx%16 + (yy%16) * 16])
                {
                    *(b++) = x + gwindow_x;
                    *(b++) = y + gwindow_y;
                    *(b++) = s[0]; //ch

                    unsigned char bg   = s[2];
                    unsigned char bold = ((s[3]&0x0f) != 0) * 8;
                    unsigned char fg   = (s[1] + bold) % 16;

                    *(b++) = fg | (bg << 4);

                    rblk->data[xx%16 + (yy%16) * 16] = *is;

                    //TODO: if we've reached limit, should send the rest the next tick and not the next frame !
                    if (++cnt >= 5000)
                        goto enough;
                }
            }
        }

        enough:;
    }

    if (b != emptyb+1 || send_z)
    {
        *firstb |= (1 << 3);
        *emptyb = zlevel;//send_z ? zlevel : 0xff;
        send_z = false;
    }
    else
        b--;

    if (*firstb)
    {
        sendfunc(buf, (int)(b-buf), conn);
        return true;
    }

    return false;
}

void process_client_cmd(const unsigned char *mdata, int msz, send_func sendfunc, void *conn)
{
    if (msz < 3)
        return;

    unsigned char cmd = mdata[0], subcmd = 0;
    if (cmd >= 0x80)
    {
        subcmd = mdata[1];
        mdata++;
        msz--;
    }
    unsigned short seq = *(unsigned short*)&mdata[1];

    bool foreign = (conn != client_peer);

    //TODO: should check for mapview instead
    if (!foreign)
    {
        if (cmd == 30 || cmd == 31)
        {
            int nx = mdata[3], ny = mdata[4];

            nx = std::max(0, nx);
            ny = std::max(0, ny);

            wx = nx;
            wy = ny;

            if (msz > 6)
                *df::global::window_z = (mdata[5] | (mdata[6] << 8)); //TODO: wz =

            if (cmd == 31)
                df::global::ui->follow_unit = -1;
            return;
        }
        
        if (cmd == 15)
        {
            FastCoreSuspender suspend;

            newwidth = std::min((int)mdata[3], world->map.x_count);
            newheight = std::min((int)mdata[4], world->map.y_count);

            waiting_render = true;
            return;
        }
        
        if (cmd == 17)
        {
            send_initial_map(seq, mdata[3], sendfunc, conn);
            return;
        }    

        if (cmd == 90)
        {
            map_render_enabled = false;
            return;
        }
    }

    FastCoreSuspender suspend;

    Lua::PushModulePublic(*out2, L, "remote", "handle_command");
    lua_pushinteger(L, cmd);
    lua_pushinteger(L, subcmd);
    lua_pushinteger(L, seq);
    lua_pushlstring(L, (const char*)mdata+3, msz-3);
    lua_pushboolean(L, foreign);

    bool handled = false;
    if (Lua::SafeCall(*out2, L, 5, 2, true))
        handled = lua_toboolean(L,-2);

    if (handled)
    {
        size_t len;
        const char *s = lua_tolstring(L, -1, &len);
        if (s && seq)
            sendfunc((const unsigned char*)s, len, conn);
    }
    else if (seq)
    {
        unsigned char *b = buf;
        *(b++) = 128;

        *(unsigned short*)b = seq + 1;
        b += 2;

        sendfunc(buf, b-buf, conn);
    }
}

void process_mediation_cmd(const unsigned char *mdata, int msz)
{
    if (msz != 4 + sizeof(ENetAddress))
        return;

    if (*(unsigned int*)&mdata[0] == 'CNCT')
    {
        ENetAddress *clientaddr = (ENetAddress*)&mdata[4];

        *out2 << "mediation server asked to connect to client at " << address2ip(clientaddr) << ":" << clientaddr->port << std::endl;
        enet_host_connect (server, clientaddr, 2, 0);
    }
}

void send_publish_cmd()
{
    if (!publish_name.size())
        return;

    std::string pubid = publish_name + pwd_hash;
    std::string pubhash = hash_password(pubid);

    int sz = 4 + pubhash.size() + 1;

    *(unsigned int*)&buf[0] = 'PUBL';
    strcpy((char*)buf + 4, pubhash.c_str());

    ENetPacket *packet = enet_packet_create (buf, sz, ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(mediation_peer, 0, packet);
}

void generate_new_server_token()
{
#ifdef arc4random
    server_token = arc4random();
#else
    srand(time(NULL));        
    server_token = rand();
#endif 
}

void generate_new_world_token()
{
#ifdef arc4random
    world_token = arc4random();
#else
    srand(time(NULL));        
    world_token = rand();
#endif 
}

void enthreadmain(ENetHost *server)
{
    ENetEvent event;
    unsigned int t = 0;
    unsigned int t2 = 0;
    bool force_send_map = false;

    while (remote_on)
    {
        if (enet_host_service (server, &event, 20) > 0)
        {
            switch (event.type)
            {
            case ENET_EVENT_TYPE_CONNECT:
                *out2 << "connected " << address2ip(&event.peer->address) << std::endl;

                if (event.peer == mediation_peer)
                {
                    *out2 << "mediation peer connected" << std::endl;
                    mediation_connected = true;
                    send_publish_cmd();
                }
                break;
            case ENET_EVENT_TYPE_RECEIVE:
                {
                    const unsigned char *mdata = event.packet->data;
                    int msz = event.packet -> dataLength;

                    if (event.peer == mediation_peer)
                        process_mediation_cmd(event.packet->data, event.packet -> dataLength);

                    else if (event.peer != client_peer || mdata[0] == 0x01)
                    {
                        // Support receiving hello from an authenticated client (if it thinks it's been disconnected)
                        if (event.peer == client_peer)
                            client_peer = NULL;

                        // Hello = cmd(1) + seq(2) + token(4) + ver(1+) + hash(1+)
                        if (event.packet->dataLength >= 1+2+4+1+1 && mdata[0] == 0x01)
                        {
                            unsigned short seq = *(unsigned short*)&mdata[1]; //1-2
                            unsigned int token = *(unsigned int*)&mdata[3]; //3-6

                            size_t verlen = 0, hashlen = 0;
                            for (int j = 7; mdata[j] && j < msz; j++, verlen++) {};
                            for (int j = 7 + verlen + 1; mdata[j] && j < msz; j++, hashlen++) {};
                            string ver((const char*)mdata + 7, verlen);
                            string hash((const char*)mdata + 7 + verlen + 1, hashlen);

                            if (pwd_hash.size() && pwd_hash != hash)
                            {
                                unsigned char *b = buf;
                                *(b++) = 128;
                                *(unsigned short*)b = seq + 1;
                                b += 2;
                                *(b++) = -1; // 3
                                *(unsigned int*)b = 0; // 4-7
                                b += 4;
                                *(b++) = 0;

                                ENetPacket * packet = enet_packet_create (buf, b-buf, ENET_PACKET_FLAG_RELIABLE);
                                enet_peer_send (event.peer, 0, packet);
                            }
                            else
                            {
                                const char *server_ver = NULL;
                                Lua::PushModulePublic(*out2, L, "remote", "matching_version");
                                lua_pushlstring(L, ver.c_str(), ver.size());
                                lua_pushboolean(L, true);
                                if (Lua::SafeCall(*out2, L, 2, 1, false))
                                    server_ver = lua_tolstring(L, -1, NULL);

                                //TODO: disconnect peer if !server_ver, matching_version should return nil if version string is invalid
                                if (!server_ver)
                                    server_ver = "";

                                bool match = (server_token != 0);
                                if (token != server_token)
                                {
                                    match = false;

                                    // Empty outgoing queue
                                    std::list<ENetPacket*> empty;
                                    std::swap(outgoing, empty);

                                    map_render_enabled = false;        
                                    force_send_status = true;        
                                }

                                if (client_peer)
                                    enet_peer_disconnect(client_peer, 1);
                                client_peer = event.peer;
                                enet_peer_ping_interval(client_peer, 1000);

                                client_addr = address2ip(&client_peer->address);

                                generate_new_server_token();

                                unsigned char *b = buf;
                                *(b++) = 128;
                                *(unsigned short*)b = seq + 1; // 1-2
                                b += 2;
                                *(b++) = match; // 3
                                *(unsigned int*)b = server_token; // 4-7
                                b += 4;
                                strcpy((char*)b, server_ver);
                                b += strlen(server_ver) + 1;

                                ENetPacket * packet = enet_packet_create (buf, b-buf, ENET_PACKET_FLAG_RELIABLE);
                                enet_peer_send (event.peer, 0, packet);

                                if (outgoing.size())
                                    resend_outgoing();
                            }
                        }
                        else
                            process_client_cmd(mdata, msz, (send_func)send_enet, event.peer);
                    }

                    //TODO: return the condition back! but only if on the game screen! because for some reason it's set to true right after game launch
                    else if (1||!df::global::ui->main.autosave_request)
                    {
                        process_client_cmd(mdata, msz, (send_func)send_enet, event.peer);
                        
                        if (!df::global::ui->main.autosave_request)
                        {
                            //TODO: only on the game screen
                            force_send_map = true;
                            //TODO: only if moved, zlevel changed, cursor moved, etc.
                            core_force_render();
                            //df::global::gview->view.child->render();
                            enet_host_flush(server);
                        }
                    }

                    enet_packet_destroy(event.packet);
                    break;
                }

            case ENET_EVENT_TYPE_DISCONNECT:
                if (event.peer == mediation_peer)
                {
                    if (mediation_connected)
                        *out2 << "disconnected from mediation server" << std::endl;
                    mediation_connected = false;
                }
                else if (client_peer == event.peer)
                    *out2 << "disconnected client" << std::endl;
                else
                    *out2 << "disconnected " << address2ip(&event.peer->address) << std::endl;

                if (client_peer == event.peer)
                {
                    client_peer = NULL;
                    *df::global::pause_state = true;
                }
                else if (event.peer == mediation_peer)
                    mediation_peer = NULL;
                break;

            case ENET_EVENT_TYPE_NONE:
                break;
            }
        }

        if (timer_timeout > 0 && --timer_timeout == 0)
        {
            lua_getglobal(L, timer_fn.c_str());
            if (!lua_isnil(L,-1))
            {
                FastCoreSuspender suspend;                
                Lua::SafeCall(*out2, L, 0, 0, true);    
            }
        }        

        if (client_peer)
        {
            if (df::global::ui->main.autosave_request)
            {
                if (old_status1 != 99)
                {
                    unsigned char buf[] = { 1, 99, 0 };
                    send_enet(buf, sizeof(buf), client_peer);
                    old_status1 = 99;
                }
            }

            else if (enabler->gframe_last != t2/*(enabler->gframe_last != t2 && force_send_map) || server->serviceTime-t >= 250*/)
            {
                //TODO: and only if on the game screen
                if (map_render_enabled)
                {
                    /*{
                        CoreSuspender suspend;
                        render_remote_map();
                    }*/

                    //*out2 << "sending map" << std::endl;
                    //*out2 << server->serviceTime << " " << (server->serviceTime-t) << std::endl;
                    t = server->serviceTime;
                    t2 = enabler->gframe_last;
                    force_send_map = false;

                    if (send_map_updates((send_func)send_enet, client_peer))
                        enet_host_flush(server);
                }
            }
        }

        if (!mediation_peer && *publish_name.data() && server->serviceTime - last_publish_attempt > 5000)
            remote_publish(publish_name);
    }

    if (client_peer)
    {
        enet_peer_disconnect (client_peer, 1);

        // Wait for the disconnection notification to be delivered
        while (enet_host_service (server, &event, 3000) > 0)
        {
            switch (event.type)
            {
            case ENET_EVENT_TYPE_RECEIVE:
                enet_packet_destroy (event.packet);
                break;
            case ENET_EVENT_TYPE_DISCONNECT:
                if (event.peer == client_peer)
                    goto end;
                break;
            }
        }
        end:;
        
        enet_peer_reset (client_peer);
        client_peer = NULL;
    }
     
    if (mediation_peer)
    {   
        enet_peer_reset (mediation_peer);
        mediation_peer = NULL;
    }

    enet_host_destroy(server);
}

void remote_start()
{
    if (remote_on)
        return;

    gmenu_w = -1;
    generate_new_server_token();
    generate_new_world_token();

    enet_initialize ();

    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = enet_port;

    server = enet_host_create (&address, 32, 2, 0, 0);

    if (!server)
    {
        *out2 << "Error starting DF Remote Server" << std::endl;
        return;
    }

    {
        std::vector <std::string> args;
        args.push_back("0");
        if (Core::getInstance().runCommand(*out2, "multilevel", args) == CR_OK)
            *out2 << "Disabled multilevel rendering, which is not supported" << std::endl;
    }

    {
        std::vector <std::string> args;
        args.push_back("disable");
        if (Core::getInstance().runCommand(*out2, "workflow", args) == CR_OK)
            *out2 << "Disabled workflow plugin, which is interfering with Remote" << std::endl;
    }

    {
        std::vector <std::string> args;
        args.push_back("disable");
        if (Core::getInstance().runCommand(*out2, "menu-mouse", args) == CR_OK)
            *out2 << "Disabled menu-mouse plugin, which is potentially interfering with Remote" << std::endl;
    }

    *out2 << "Dwarf Fortress Remote server listening on port " << enet_port << std::endl;

    wx = *df::global::window_x;
    wy = *df::global::window_y;

    enabler->gfps = 5;

    INTERPOSE_HOOK(dwarfmode_hook2, render).apply(true);
    //INTERPOSE_HOOK(dwarfmode_hook2, feed).apply(true);

    remote_on = true;

    enthread = new tthread::thread((void (*)(void *))enthreadmain, server);
}

void remote_stop()
{
    if (!remote_on)
        return;

    *df::global::pause_state = true;

    INTERPOSE_HOOK(dwarfmode_hook2, render).apply(false);
    //INTERPOSE_HOOK(dwarfmode_hook2, feed).apply(false);

    remote_on = false;
    timer_timeout = -1;

    enthread->join();
    delete enthread;
    enthread = NULL;

    enabler->gfps = 50;
}

void remote_publish(string &name)
{
    publish_name = name;

    if (!remote_on || !name.size())
        return;

    if (mediation_peer)
    {
        enet_peer_reset(mediation_peer);
        mediation_peer = NULL;
    }

    last_publish_attempt = enet_time_get();

    ENetAddress mediation_address;
    enet_address_set_host (&mediation_address, "dfmed.mifki.com");
    mediation_address.port = 1233;

    mediation_peer = enet_host_connect (server, &mediation_address, 2, 0);
    enet_peer_ping_interval(mediation_peer, 5000);
}

void remote_unpublish()
{
    if (!remote_on)
        return;

    if (mediation_peer)
    {
        enet_peer_reset(mediation_peer);
        mediation_peer = NULL;
    }

    publish_name = "";    
}

void remote_setpwd(string &pwd)
{
    if (pwd.size())
        pwd_hash = hash_password(pwd);
    else
        pwd_hash = "";

    // Force re-publish right now
    remote_publish(publish_name);
}

void remote_unload()
{
    Lua::PushModulePublic(*out2, L, "remote", "unload");
    Lua::SafeCall(*out2, L, 0, 0, true);
}

bool remote_print_version()
{
    const char *s = NULL;

    Lua::PushModulePublic(*out2, L, "remote", "get_version");
    if (Lua::SafeCall(*out2, L, 0, 1, false))
        s = lua_tolstring(L, -1, NULL);

    if (s)
    {
        *out2 << COLOR_LIGHTGREEN << "Dwarf Fortress Remote version " << s << std::endl;
        *out2 << COLOR_LIGHTGREEN << "Type `help remote` for help." << std::endl;
        *out2 << COLOR_RESET;

        return true;
    }
    else
    {
        out2->printerr("Dwarf Fortress Remote scripts couldn't be loaded, execute `remote-update` command\n");
        out2->printerr("Or visit http://mifki.com/df/setup for manual installation instructions\n");

        return false;
    }
}

void set_timer(int timeout, string fn)
{
    timer_timeout = timeout * 1000 / 20;
    timer_fn = fn;
}

bool verify_pwd(string pwdhash)
{
    return (pwd_hash == pwdhash || (pwd_hash.size() == 0 && pwdhash.size() == 0));
}

bool check_wtoken(unsigned int wtoken)
{
    bool match = (wtoken == world_token);

    if (!match)
    {
        empty_map_cache();
    }

    return match;
}

unsigned int update_wtoken()
{
    generate_new_world_token();

    return world_token;
}

bool start_update()
{
    std::vector <std::string> args;
    args.push_back("delayed");
    return (Core::getInstance().runCommand(*out2, "remote-update", args) == CR_OK);
}

static std::string custom_command(std::string data)
{
    return NULL;
}

#include "config.hpp"
#include "commands.hpp"
#include "plugin.hpp"
