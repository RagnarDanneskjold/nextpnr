/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  Clifford Wolf <clifford@symbioticeda.com>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

assertion_failure::assertion_failure(std::string msg, std::string expr_str, std::string filename, int line)
        : runtime_error("Assertion failure: " + msg + " (" + filename + ":" + std::to_string(line) + ")"), msg(msg),
          expr_str(expr_str), filename(filename), line(line)
{
}

void IdString::set(const BaseCtx *ctx, const std::string &s)
{
    auto it = ctx->idstring_str_to_idx->find(s);
    if (it == ctx->idstring_str_to_idx->end()) {
        index = ctx->idstring_idx_to_str->size();
        auto insert_rc = ctx->idstring_str_to_idx->insert({s, index});
        ctx->idstring_idx_to_str->push_back(&insert_rc.first->first);
    } else {
        index = it->second;
    }
}

const std::string &IdString::str(const BaseCtx *ctx) const { return *ctx->idstring_idx_to_str->at(index); }

const char *IdString::c_str(const BaseCtx *ctx) const { return str(ctx).c_str(); }

void IdString::initialize_add(const BaseCtx *ctx, const char *s, int idx)
{
    NPNR_ASSERT(ctx->idstring_str_to_idx->count(s) == 0);
    NPNR_ASSERT(int(ctx->idstring_idx_to_str->size()) == idx);
    auto insert_rc = ctx->idstring_str_to_idx->insert({s, idx});
    ctx->idstring_idx_to_str->push_back(&insert_rc.first->first);
}

WireId Context::getNetinfoSourceWire(const NetInfo *net_info) const
{
    if (net_info->driver.cell == nullptr)
        return WireId();

    auto src_bel = net_info->driver.cell->bel;

    if (src_bel == BelId())
        return WireId();

    IdString driver_port = net_info->driver.port;

    auto driver_port_it = net_info->driver.cell->pins.find(driver_port);
    if (driver_port_it != net_info->driver.cell->pins.end())
        driver_port = driver_port_it->second;

    return getBelPinWire(src_bel, portPinFromId(driver_port));
}

WireId Context::getNetinfoSinkWire(const NetInfo *net_info, const PortRef &user_info) const
{
    auto dst_bel = user_info.cell->bel;

    if (dst_bel == BelId())
        return WireId();

    IdString user_port = user_info.port;

    auto user_port_it = user_info.cell->pins.find(user_port);

    if (user_port_it != user_info.cell->pins.end())
        user_port = user_port_it->second;

    return getBelPinWire(dst_bel, portPinFromId(user_port));
}

delay_t Context::getNetinfoRouteDelay(const NetInfo *net_info, const PortRef &user_info) const
{
    WireId src_wire = getNetinfoSourceWire(net_info);
    if (src_wire == WireId())
        return 0;

    WireId dst_wire = getNetinfoSinkWire(net_info, user_info);
    WireId cursor = dst_wire;
    delay_t delay = 0;

    while (cursor != WireId() && cursor != src_wire) {
        auto it = net_info->wires.find(cursor);
        if (it == net_info->wires.end())
            break;
        PipId pip = it->second.pip;
        delay += getPipDelay(pip).maxDelay();
        delay += getWireDelay(cursor).maxDelay();
        cursor = getPipSrcWire(pip);
    }

    if (cursor == src_wire)
        return delay + getWireDelay(src_wire).maxDelay();

    return predictDelay(net_info, user_info);
}

static uint32_t xorshift32(uint32_t x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

uint32_t Context::checksum() const
{
    uint32_t cksum = xorshift32(123456789);

    uint32_t cksum_nets_sum = 0;
    for (auto &it : nets) {
        auto &ni = *it.second;
        uint32_t x = 123456789;
        x = xorshift32(x + xorshift32(it.first.index));
        x = xorshift32(x + xorshift32(ni.name.index));
        if (ni.driver.cell)
            x = xorshift32(x + xorshift32(ni.driver.cell->name.index));
        x = xorshift32(x + xorshift32(ni.driver.port.index));
        x = xorshift32(x + xorshift32(getDelayChecksum(ni.driver.budget)));

        for (auto &u : ni.users) {
            if (u.cell)
                x = xorshift32(x + xorshift32(u.cell->name.index));
            x = xorshift32(x + xorshift32(u.port.index));
            x = xorshift32(x + xorshift32(getDelayChecksum(u.budget)));
        }

        uint32_t attr_x_sum = 0;
        for (auto &a : ni.attrs) {
            uint32_t attr_x = 123456789;
            attr_x = xorshift32(attr_x + xorshift32(a.first.index));
            for (uint8_t ch : a.second)
                attr_x = xorshift32(attr_x + xorshift32(ch));
            attr_x_sum += attr_x;
        }
        x = xorshift32(x + xorshift32(attr_x_sum));

        uint32_t wire_x_sum = 0;
        for (auto &w : ni.wires) {
            uint32_t wire_x = 123456789;
            wire_x = xorshift32(wire_x + xorshift32(getWireChecksum(w.first)));
            wire_x = xorshift32(wire_x + xorshift32(getPipChecksum(w.second.pip)));
            wire_x = xorshift32(wire_x + xorshift32(int(w.second.strength)));
            wire_x_sum += wire_x;
        }
        x = xorshift32(x + xorshift32(wire_x_sum));

        cksum_nets_sum += x;
    }
    cksum = xorshift32(cksum + xorshift32(cksum_nets_sum));

    uint32_t cksum_cells_sum = 0;
    for (auto &it : cells) {
        auto &ci = *it.second;
        uint32_t x = 123456789;
        x = xorshift32(x + xorshift32(it.first.index));
        x = xorshift32(x + xorshift32(ci.name.index));
        x = xorshift32(x + xorshift32(ci.type.index));

        uint32_t port_x_sum = 0;
        for (auto &p : ci.ports) {
            uint32_t port_x = 123456789;
            port_x = xorshift32(port_x + xorshift32(p.first.index));
            port_x = xorshift32(port_x + xorshift32(p.second.name.index));
            if (p.second.net)
                port_x = xorshift32(port_x + xorshift32(p.second.net->name.index));
            port_x = xorshift32(port_x + xorshift32(p.second.type));
            port_x_sum += port_x;
        }
        x = xorshift32(x + xorshift32(port_x_sum));

        uint32_t attr_x_sum = 0;
        for (auto &a : ci.attrs) {
            uint32_t attr_x = 123456789;
            attr_x = xorshift32(attr_x + xorshift32(a.first.index));
            for (uint8_t ch : a.second)
                attr_x = xorshift32(attr_x + xorshift32(ch));
            attr_x_sum += attr_x;
        }
        x = xorshift32(x + xorshift32(attr_x_sum));

        uint32_t param_x_sum = 0;
        for (auto &p : ci.params) {
            uint32_t param_x = 123456789;
            param_x = xorshift32(param_x + xorshift32(p.first.index));
            for (uint8_t ch : p.second)
                param_x = xorshift32(param_x + xorshift32(ch));
            param_x_sum += param_x;
        }
        x = xorshift32(x + xorshift32(param_x_sum));

        x = xorshift32(x + xorshift32(getBelChecksum(ci.bel)));
        x = xorshift32(x + xorshift32(ci.belStrength));

        uint32_t pin_x_sum = 0;
        for (auto &a : ci.pins) {
            uint32_t pin_x = 123456789;
            pin_x = xorshift32(pin_x + xorshift32(a.first.index));
            pin_x = xorshift32(pin_x + xorshift32(a.second.index));
            pin_x_sum += pin_x;
        }
        x = xorshift32(x + xorshift32(pin_x_sum));

        cksum_cells_sum += x;
    }
    cksum = xorshift32(cksum + xorshift32(cksum_cells_sum));

    return cksum;
}

void Context::check() const
{
    for (auto &n : nets) {
        auto ni = n.second.get();
        NPNR_ASSERT(n.first == ni->name);
        for (auto &w : ni->wires) {
            NPNR_ASSERT(n.first == getBoundWireNet(w.first));
            if (w.second.pip != PipId()) {
                NPNR_ASSERT(w.first == getPipDstWire(w.second.pip));
                NPNR_ASSERT(n.first == getBoundPipNet(w.second.pip));
            }
        }
    }

    for (auto w : getWires()) {
        IdString net = getBoundWireNet(w);
        if (net != IdString()) {
            NPNR_ASSERT(nets.at(net)->wires.count(w));
        }
    }

    for (auto &c : cells) {
        NPNR_ASSERT(c.first == c.second->name);
        if (c.second->bel != BelId())
            NPNR_ASSERT(getBoundBelCell(c.second->bel) == c.first);
        for (auto &port : c.second->ports) {
            NetInfo *net = port.second.net;
            if (net != nullptr) {
                NPNR_ASSERT(nets.find(net->name) != nets.end());
                if (port.second.type == PORT_OUT) {
                    NPNR_ASSERT(net->driver.cell == c.second.get() && net->driver.port == port.first);
                } else if (port.second.type == PORT_IN) {
                    NPNR_ASSERT(std::count_if(net->users.begin(), net->users.end(), [&](const PortRef &pr) {
                                    return pr.cell == c.second.get() && pr.port == port.first;
                                }) == 1);
                }
            }
        }
    }
}

NEXTPNR_NAMESPACE_END
