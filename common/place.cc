/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  Clifford Wolf <clifford@clifford.at>
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

#include "place.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <list>
#include <map>
#include <ostream>
#include <queue>
#include <set>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "arch_place.h"
#include "log.h"

NEXTPNR_NAMESPACE_BEGIN

void place_design(Design *design)
{
    std::set<IdString> types_used;
    std::set<IdString>::iterator not_found, element;
    std::set<BelType> used_bels;

    log_info("Placing..\n");

    // Initial constraints placer
    for (auto cell_entry : design->cells) {
        CellInfo *cell = cell_entry.second;
        auto loc = cell->attrs.find("BEL");
        if (loc != cell->attrs.end()) {
            std::string loc_name = loc->second;
            BelId bel = design->chip.getBelByName(IdString(loc_name));
            if (bel == BelId()) {
                log_error("No Bel named \'%s\' located for "
                          "this chip (processing BEL attribute on \'%s\')\n",
                          loc_name.c_str(), cell->name.c_str());
            }

            BelType bel_type = design->chip.getBelType(bel);
            if (bel_type != belTypeFromId(cell->type)) {
                log_error("Bel \'%s\' of type \'%s\' does not match cell "
                          "\'%s\' of type \'%s\'",
                          loc_name.c_str(), belTypeToId(bel_type).c_str(),
                          cell->name.c_str(), cell->type.c_str());
            }

            cell->bel = bel;
            design->chip.bindBel(bel, cell->name);
        }
    }

    for (auto cell_entry : design->cells) {
        CellInfo *cell = cell_entry.second;
        // Ignore already placed cells
        if (cell->bel != BelId())
            continue;

        BelType bel_type;

        element = types_used.find(cell->type);
        if (element != types_used.end()) {
            continue;
        }

        bel_type = belTypeFromId(cell->type);
        if (bel_type == BelType()) {
            log_error("No Bel of type \'%s\' defined for "
                      "this chip\n",
                      cell->type.c_str());
        }
        types_used.insert(cell->type);
    }

    for (auto bel_type_name : types_used) {
        auto blist = design->chip.getBels();
        BelType bel_type = belTypeFromId(bel_type_name);
        auto bi = blist.begin();

        for (auto cell_entry : design->cells) {
            CellInfo *cell = cell_entry.second;

            // Ignore already placed cells
            if (cell->bel != BelId())
                continue;
            // Only place one type of Bel at a time
            if (cell->type != bel_type_name)
                continue;

            while ((bi != blist.end()) &&
                   ((design->chip.getBelType(*bi) != bel_type ||
                     !design->chip.checkBelAvail(*bi)) ||
                    !isValidBelForCell(design, cell, *bi)))
                bi++;
            if (bi == blist.end())
                log_error("Too many \'%s\' used in design\n",
                          cell->type.c_str());
            cell->bel = *bi++;
            design->chip.bindBel(cell->bel, cell->name);

            // Back annotate location
            cell->attrs["BEL"] = design->chip.getBelName(cell->bel).str();
        }
    }
}

static void place_cell(Design *design, CellInfo *cell)
{

    float best_distance = std::numeric_limits<float>::infinity();
    BelId best_bel = BelId();
    Chip &chip = design->chip;
    if(cell->bel != BelId()) {
        chip.unbindBel(cell->bel);
        cell->bel = BelId();
    }
    BelType targetType = belTypeFromId(cell->type);
    for (auto bel : chip.getBels()) {
        if (chip.getBelType(bel) == targetType && chip.checkBelAvail(bel) &&
            isValidBelForCell(design, cell, bel)) {
            float distance = 0;
            float belx, bely;
            chip.estimatePosition(bel, belx, bely);
            for (auto port : cell->ports) {
                const PortInfo &pi = port.second;
                if (pi.net != nullptr) {
                    CellInfo *drv = pi.net->driver.cell;
                    if (drv != nullptr && drv->bel != BelId()) {
                        float otherx, othery;
                        chip.estimatePosition(drv->bel, otherx, othery);
                        distance += std::abs(belx - otherx) +
                                    std::abs(bely - othery);
                    }
                    if (pi.net->users.size() < 5) {
                        for (auto user : pi.net->users) {
                            CellInfo *uc = user.cell;
                            if (uc != nullptr && uc->bel != BelId()) {
                                float otherx, othery;
                                chip.estimatePosition(uc->bel, otherx, othery);
                                distance += std::abs(belx - otherx) +
                                            std::abs(bely - othery);
                            }
                        }
                    }
                }
            }
            if (distance <= best_distance) {
                best_distance = distance;
                best_bel = bel;
            }
        }
    }
    if (best_bel == BelId()) {
        log_error("failed to place cell '%s' of type '%s'\n",
                  cell->name.c_str(), cell->type.c_str());
    }
    cell->bel = best_bel;
    chip.bindBel(cell->bel, cell->name);

    // Back annotate location
    cell->attrs["BEL"] = chip.getBelName(cell->bel).str();
}

void place_design_heuristic(Design *design)
{
    size_t total_cells = design->cells.size(), placed_cells = 0;
    std::queue<CellInfo *> visit_cells;
    // Initial constraints placer
    for (auto cell_entry : design->cells) {
        CellInfo *cell = cell_entry.second;
        auto loc = cell->attrs.find("BEL");
        if (loc != cell->attrs.end()) {
            std::string loc_name = loc->second;
            BelId bel = design->chip.getBelByName(IdString(loc_name));
            if (bel == BelId()) {
                log_error("No Bel named \'%s\' located for "
                          "this chip (processing BEL attribute on \'%s\')\n",
                          loc_name.c_str(), cell->name.c_str());
            }

            BelType bel_type = design->chip.getBelType(bel);
            if (bel_type != belTypeFromId(cell->type)) {
                log_error("Bel \'%s\' of type \'%s\' does not match cell "
                          "\'%s\' of type \'%s\'",
                          loc_name.c_str(), belTypeToId(bel_type).c_str(),
                          cell->name.c_str(), cell->type.c_str());
            }

            cell->bel = bel;
            design->chip.bindBel(bel, cell->name);
            placed_cells++;
            visit_cells.push(cell);
        }
    }
    log_info("place_constraints placed %d\n", placed_cells);
    std::unordered_set<IdString> autoplaced;
    for (auto cell : design->cells) {
        CellInfo *ci = cell.second;
        if (ci->bel == BelId()) {
            place_cell(design, ci);
            autoplaced.insert(cell.first);
            placed_cells++;
        }
        log_info("placed %d/%d\n", placed_cells, total_cells);
    }
    for (int i = 0 ; i < 3; i ++) {
        int replaced_cells = 0;
        for (auto cell : autoplaced) {
            CellInfo *ci = design->cells[cell];
            place_cell(design, ci);
            replaced_cells++;
            log_info("replaced %d/%d\n", replaced_cells, autoplaced.size());
        }
    }

}

NEXTPNR_NAMESPACE_END
