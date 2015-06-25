/**
 * @file snmp_accumulator_table.h
 *
 * Project Clearwater - IMS in the Cloud
 * Copyright (C) 2015 Metaswitch Networks Ltd
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version, along with the "Special Exception" for use of
 * the program along with SSL, set forth below. This program is distributed
 * in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details. You should have received a copy of the GNU General Public
 * License along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * The author can be reached by email at clearwater@metaswitch.com or by
 * post at Metaswitch Networks Ltd, 100 Church St, Enfield EN2 6BQ, UK
 *
 * Special Exception
 * Metaswitch Networks Ltd  grants you permission to copy, modify,
 * propagate, and distribute a work formed by combining OpenSSL with The
 * Software, or a work derivative of such a combination, even if such
 * copying, modification, propagation, or distribution would otherwise
 * violate the terms of the GPL. You must comply with the GPL in all
 * respects for all of the code used other than OpenSSL.
 * "OpenSSL" means OpenSSL toolkit software distributed by the OpenSSL
 * Project and licensed under the OpenSSL Licenses, or a work based on such
 * software and licensed und er the OpenSSL Licenses.
 * "OpenSSL Licenses" means the OpenSSL License and Original SSLeay License
 * under which the OpenSSL Project distributes the OpenSSL toolkit software,
 * as those licenses appear in the file LICENSE-OPENSSL.
 */

#include <vector>
#include <map>
#include <string>
#include <atomic>

#include "snmp_includes.h"
#include "snmp_time_period_table.h"
#include "logger.h"

#ifndef SNMP_ACCUMULATOR_TABLE_H
#define SNMP_ACCUMULATOR_TABLE_H

// This file contains infrastructure for tables which:
//   - are indexed by time period
//   - accumulate data samples over time
//   - report a count of samples, mean sample value, variance, high-water-mark and low-water-mark
//
// The thing sampled doesn't matter - it could be latency, size of a queue, etc.
//
// To create an accumulator table, simply create one, and call `accumulate` on it as data comes in,
// e.g.:
//
// AccumulatorTable* bono_latency_table = new AccumulatorTable("bono_latency", 
//                                                             my_oid,
//                                                             OID_LENGTH(my_oid));
// bono_latency_table->accumulate(2000);
//
// This will create AccumulatorRow and Statistics classes internally (the user doesn't need to
// handle those).


namespace SNMP
{

// Storage for the underlying data
struct Statistics
{
  std::atomic_uint_fast64_t count;
  std::atomic_uint_fast64_t sum;
  std::atomic_uint_fast64_t sqsum;
  std::atomic_uint_fast64_t hwm;
  std::atomic_uint_fast64_t lwm;

  void reset();
};

// Just a TimeBasedRow that maps the data from Statistics into the right five columns.
class AccumulatorRow: public TimeBasedRow<Statistics>
{
public:
  AccumulatorRow(int index, View* view): TimeBasedRow<Statistics>(index, view) {};
  ColumnData get_columns();
};

class AccumulatorTable: public ManagedTable<AccumulatorRow, int>
{
public:
  AccumulatorTable(std::string name,
                   oid* tbl_oid,
                   int oidlen) :
    ManagedTable<AccumulatorRow, int>(name,
                                      tbl_oid,
                                      oidlen,
                                      2,
                                      6, // Columns 2-6 should be visible
                                      { ASN_INTEGER }), // Type of the index column
    five_second(5),
    five_minute(300)
  {
    // We have a fixed number of rows, so create them in the constructor.
    add(TimePeriodIndexes::scopePrevious5SecondPeriod);
    add(TimePeriodIndexes::scopeCurrent5MinutePeriod);
    add(TimePeriodIndexes::scopePrevious5MinutePeriod);
  }

  // Accumulate a sample into the underlying statistics.
  void accumulate(uint32_t sample)
  {
    // Pass samples through to the underlying data structures
    accumulate_internal(five_second, sample);
    accumulate_internal(five_minute, sample);
  }

private:
  // Map row indexes to the view of the underlying data they should expose
  AccumulatorRow* new_row(int index)
  {
    AccumulatorRow::View* view = NULL;
    switch (index)
    {
      case TimePeriodIndexes::scopePrevious5SecondPeriod:
        view = new AccumulatorRow::PreviousView(&five_second);
        break;
      case TimePeriodIndexes::scopeCurrent5MinutePeriod:
        view = new AccumulatorRow::CurrentView(&five_minute);
        break;
      case TimePeriodIndexes::scopePrevious5MinutePeriod:
        view = new AccumulatorRow::PreviousView(&five_minute);
        break;
    }
    return new AccumulatorRow(index, view);
  }

  void accumulate_internal(AccumulatorRow::CurrentAndPrevious& data, uint32_t sample);

  AccumulatorRow::CurrentAndPrevious five_second;
  AccumulatorRow::CurrentAndPrevious five_minute;
};

}

#endif
