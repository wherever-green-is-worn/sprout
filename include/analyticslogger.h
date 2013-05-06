/**
 * @file analyticslogger.h Declaration of AnalyticsLogger class.
 *
 * Copyright (C) 2013  Metaswitch Networks Ltd
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * The author can be reached by email at clearwater@metaswitch.com or by post at
 * Metaswitch Networks Ltd, 100 Church St, Enfield EN2 6BQ, UK
 */

///
///

#ifndef ANALYTICSLOGGER_H__
#define ANALYTICSLOGGER_H__

#include <sstream>

#include "logger.h"

class AnalyticsLogger
{
public:
  AnalyticsLogger(const std::string& directory);
  ~AnalyticsLogger();

  void registration(const std::string& aor,
                    const std::string& binding_id,
                    const std::string& contact,
                    int expires);

  void auth_failure(const std::string& uri);

  void call_connected(const std::string& from,
                      const std::string& to,
                      const std::string& call_id);

  void call_not_connected(const std::string& from,
                          const std::string& to,
                          const std::string& call_id,
                          int reason);

  void call_disconnected(const std::string& call_id,
                         int reason);

private:
  static const int BUFFER_SIZE = 1000;

  Logger* _logger;
};

#endif
