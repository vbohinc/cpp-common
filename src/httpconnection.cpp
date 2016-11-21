/**
 * @file httpconnection.cpp HttpConnection class methods.
 *
 * Project Clearwater - IMS in the Cloud
 * Copyright (C) 2013  Metaswitch Networks Ltd
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
 * software and licensed under the OpenSSL Licenses.
 * "OpenSSL Licenses" means the OpenSSL License and Original SSLeay License
 * under which the OpenSSL Project distributes the OpenSSL toolkit software,
 * as those licenses appear in the file LICENSE-OPENSSL.
 */

#include <curl/curl.h>
#include <cassert>
#include <iostream>
#include <map>

#include "cpp_common_pd_definitions.h"
#include "utils.h"
#include "log.h"
#include "sas.h"
#include "httpconnection.h"
#include "load_monitor.h"
#include "random_uuid.h"

/// Total time to wait for a response from the server as a multiple of the
/// configured target latency before giving up.  This is the value that affects
/// the user experience, so should be set to what we consider acceptable.
/// Covers lookup, possibly multiple connection attempts, request, and
/// response.
static const int TIMEOUT_LATENCY_MULTIPLIER = 5;
//static const int DEFAULT_LATENCY_US = 100000;
static const int DEFAULT_LATENCY_US = 500000;

/// Approximate length of time to wait before giving up on a
/// connection attempt to a single address (in milliseconds).  cURL
/// may wait more or less than this depending on the number of
/// addresses to be tested and where this address falls in the
/// sequence. A connection will take longer than this to establish if
/// multiple addresses must be tried. This includes only the time to
/// perform the DNS lookup and establish the connection, not to send
/// the request or receive the response.
///
/// We set this quite short to ensure we quickly move on to another
/// server. A connection should be very fast to establish (a few
/// milliseconds) in the success case.
//static const long SINGLE_CONNECT_TIMEOUT_MS = 50;
static const long SINGLE_CONNECT_TIMEOUT_MS = 500;

/// Mean age of a connection before we recycle it. Ensures we respect
/// DNS changes, and that we rebalance load when servers come back up
/// after failure. Actual connection recycle events are
/// Poisson-distributed with this mean inter-arrival time.
static const double CONNECTION_AGE_MS = 60 * 1000.0;

/// Maximum number of targets to try connecting to.
static const int MAX_TARGETS = 5;

/// Create an HTTP connection object.
///
/// @param server Server to send HTTP requests to.
/// @param assert_user Assert user in header?
/// @param resolver HTTP resolver to use to resolve server's IP addresses
/// @param stat_name SNMP table to report connection info to.
/// @param load_monitor Load Monitor.
/// @param sas_log_level the level to log HTTP flows at (none/protocol/detail).
HttpConnection::HttpConnection(const std::string& server,
                               bool assert_user,
                               HttpResolver* resolver,
                               SNMP::IPCountTable* stat_table,
                               LoadMonitor* load_monitor,
                               SASEvent::HttpLogLevel sas_log_level,
                               BaseCommunicationMonitor* comm_monitor,
                               const std::string& scheme) :
  _server(server),
  _host(host_from_server(server)),
  _scheme(scheme),
  _port(port_from_server(server)),
  _assert_user(assert_user),
  _resolver(resolver),
  _sas_log_level(sas_log_level),
  _comm_monitor(comm_monitor),
  _stat_table(stat_table)
{
  pthread_key_create(&_curl_thread_local, cleanup_curl);
  pthread_key_create(&_uuid_thread_local, cleanup_uuid);
  pthread_mutex_init(&_lock, NULL);
  curl_global_init(CURL_GLOBAL_DEFAULT);
  std::vector<std::string> no_stats;
  _load_monitor = load_monitor;
  _timeout_ms = calc_req_timeout_from_latency((load_monitor != NULL) ?
                               load_monitor->get_target_latency_us() :
                               DEFAULT_LATENCY_US);
  TRC_STATUS("Configuring HTTP Connection");
  TRC_STATUS("  Connection created for server %s", _server.c_str());
  TRC_STATUS("  Connection will use a response timeout of %ldms", _timeout_ms);
}


/// Create an HTTP connection object.
///
/// @param server Server to send HTTP requests to.
/// @param assert_user Assert user in header?
/// @param resolver HTTP resolver to use to resolve server's IP addresses
/// @param sas_log_level the level to log HTTP flows at (none/protocol/detail).
HttpConnection::HttpConnection(const std::string& server,
                               bool assert_user,
                               HttpResolver* resolver,
                               SASEvent::HttpLogLevel sas_log_level,
                               BaseCommunicationMonitor* comm_monitor,
                               const std::string& scheme) :
  _server(server),
  _host(host_from_server(server)),
  _scheme(scheme),
  _port(port_from_server(server)),
  _assert_user(assert_user),
  _resolver(resolver),
  _sas_log_level(sas_log_level),
  _comm_monitor(comm_monitor),
  _stat_table(NULL)
{
  pthread_key_create(&_curl_thread_local, cleanup_curl);
  pthread_key_create(&_uuid_thread_local, cleanup_uuid);
  pthread_mutex_init(&_lock, NULL);
  curl_global_init(CURL_GLOBAL_DEFAULT);
  _load_monitor = NULL;
  _timeout_ms = calc_req_timeout_from_latency(DEFAULT_LATENCY_US);
  TRC_STATUS("Configuring HTTP Connection");
  TRC_STATUS("  Connection created for server %s", _server.c_str());
  TRC_STATUS("  Connection will use a response timeout of %ldms", _timeout_ms);
}

HttpConnection::~HttpConnection()
{
  // Clean up this thread's connection now, rather than waiting for
  // pthread_exit.  This is to support use by single-threaded code
  // (e.g., UTs), where pthread_exit is never called.
  CURL* curl = pthread_getspecific(_curl_thread_local);
  if (curl != NULL)
  {
    pthread_setspecific(_curl_thread_local, NULL);
    cleanup_curl(curl); curl = NULL;
  }

  RandomUUIDGenerator* uuid_gen =
    (RandomUUIDGenerator*)pthread_getspecific(_uuid_thread_local);

  if (uuid_gen != NULL)
  {
    pthread_setspecific(_uuid_thread_local, NULL);
    cleanup_uuid(uuid_gen); uuid_gen = NULL;
  }

  pthread_key_delete(_curl_thread_local);
  pthread_key_delete(_uuid_thread_local);
}

/// Get the thread-local curl handle if it exists, and create it if not.
CURL* HttpConnection::get_curl_handle()
{
  CURL* curl = pthread_getspecific(_curl_thread_local);
  if (curl == NULL)
  {
    curl = curl_easy_init();
    TRC_DEBUG("Allocated CURL handle %p", curl);
    pthread_setspecific(_curl_thread_local, curl);

    // Create our private data
    PoolEntry* entry = new PoolEntry(this);
    curl_easy_setopt(curl, CURLOPT_PRIVATE, entry);

    // Retrieved data will always be written to a string.
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &string_store);

    // Only keep one TCP connection to a Homestead per thread, to
    // avoid using unnecessary resources. We only try a different
    // Homestead when one fails, or after we've had it open for a
    // while, and in neither case do we want to keep the old
    // connection around.
    curl_easy_setopt(curl, CURLOPT_MAXCONNECTS, 1L);

    // Maximum time to wait for a response.  This is the target latency for
    // this node plus a delta
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, _timeout_ms);

    // Time to wait until we establish a TCP connection to a single host.
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, SINGLE_CONNECT_TIMEOUT_MS);

    // We mustn't reuse DNS responses, because cURL does no shuffling
    // of DNS entries and we rely on this for load balancing.
    curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, 0L);

    // Nagle is not required. Probably won't bite us, but can't hurt
    // to turn it off.
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);

    // We are a multithreaded app using C-Ares. This is the
    // recommended setting.
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    // Register a debug callback to record the HTTP transaction.  We also need
    // to set the verbose option (otherwise setting the debug function has no
    // effect).
    curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, Recorder::debug_callback);
    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
  }
  return curl;
}

// Map the CURLcode into a sensible HTTP return code.
HTTPCode HttpConnection::curl_code_to_http_code(CURL* curl, CURLcode code)
{
  switch (code)
  {
  case CURLE_OK:
  {
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    return http_code;
  // LCOV_EXCL_START
  }
  case CURLE_URL_MALFORMAT:
  case CURLE_NOT_BUILT_IN:
    return HTTP_BAD_REQUEST;
  // LCOV_EXCL_STOP
  case CURLE_REMOTE_FILE_NOT_FOUND:
    return HTTP_NOT_FOUND;
  // LCOV_EXCL_START
  case CURLE_COULDNT_RESOLVE_PROXY:
  case CURLE_COULDNT_RESOLVE_HOST:
  case CURLE_COULDNT_CONNECT:
  case CURLE_AGAIN:
    return HTTP_NOT_FOUND;
  default:
    return HTTP_SERVER_ERROR;
  // LCOV_EXCL_STOP
  }
}

// Reset the cURL handle to a default state, so that settings from one
// request don't leak into another
void HttpConnection::reset_curl_handle(CURL* curl)
{
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, NULL);
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, NULL);
  curl_easy_setopt(curl, CURLOPT_WRITEHEADER, NULL);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, NULL);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, NULL);
  curl_easy_setopt(curl, CURLOPT_POST, 0);
}

HTTPCode HttpConnection::send_delete(const std::string& path,
                                     SAS::TrailId trail,
                                     const std::string& body)
{
  std::string unused_response;
  std::map<std::string, std::string> unused_headers;

  return send_delete(path, unused_headers, unused_response, trail, body);
}

HTTPCode HttpConnection::send_delete(const std::string& path,
                                     SAS::TrailId trail,
                                     const std::string& body,
                                     const std::string& override_server)
{
  std::string unused_response;
  std::map<std::string, std::string> unused_headers;
  change_server(override_server);

  return send_delete(path, unused_headers, unused_response, trail, body);
}

HTTPCode HttpConnection::send_delete(const std::string& path,
                                     SAS::TrailId trail,
                                     const std::string& body,
                                     std::string& response)
{
  std::map<std::string, std::string> unused_headers;

  return send_delete(path, unused_headers, response, trail, body);
}

HTTPCode HttpConnection::send_delete(const std::string& path,
                                     std::map<std::string, std::string>& headers,
                                     std::string& response,
                                     SAS::TrailId trail,
                                     const std::string& body,
                                     const std::string& username)
{
  CURL *curl = get_curl_handle();
  struct curl_slist *slist = NULL;
  slist = curl_slist_append(slist, "Expect:");

  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, slist);
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");

  std::vector<std::string> unused_extra_headers;
  HTTPCode status = send_request(path, body, response, username, trail, "DELETE", unused_extra_headers, curl);

  curl_slist_free_all(slist);

  return status;
}

HTTPCode HttpConnection::send_put(const std::string& path,
                                  const std::string& body,
                                  SAS::TrailId trail,
                                  const std::string& username)
{
  std::string unused_response;
  std::map<std::string, std::string> unused_headers;
  std::vector<std::string> extra_req_headers;
  return HttpConnection::send_put(path,
                                  unused_headers,
                                  unused_response,
                                  body,
                                  extra_req_headers,
                                  trail,
                                  username);
}

HTTPCode HttpConnection::send_put(const std::string& path,
                                  std::string& response,
                                  const std::string& body,
                                  SAS::TrailId trail,
                                  const std::string& username)
{
  std::map<std::string, std::string> unused_headers;
  std::vector<std::string> extra_req_headers;
  return HttpConnection::send_put(path,
                                  unused_headers,
                                  response,
                                  body,
                                  extra_req_headers,
                                  trail,
                                  username);
}

HTTPCode HttpConnection::send_put(const std::string& path,
                                  std::map<std::string, std::string>& headers,
                                  const std::string& body,
                                  SAS::TrailId trail,
                                  const std::string& username)
{
  std::string unused_response;
  std::vector<std::string> extra_req_headers;
  return HttpConnection::send_put(path,
                                  headers,
                                  unused_response,
                                  body,
                                  extra_req_headers,
                                  trail,
                                  username);
}

HTTPCode HttpConnection::send_put(const std::string& path,                     //< Absolute path to request from server - must start with "/"
                                  std::map<std::string, std::string>& headers, //< Map of headers from the response
                                  std::string& response,                       //< Retrieved document
                                  const std::string& body,                     //< Body to send in request
                                  const std::vector<std::string>& extra_req_headers, //< Extra headers to add to the request.
                                  SAS::TrailId trail,                          //< SAS trail
                                  const std::string& username)                 //< Username to assert (if assertUser was true, else ignored)
{
  CURL *curl = get_curl_handle();
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &HttpConnection::write_headers);
  curl_easy_setopt(curl, CURLOPT_WRITEHEADER, &headers);

  HTTPCode status = send_request(path,
                                 body,
                                 response,
                                 "",
                                 trail,
                                 "PUT",
                                 extra_req_headers,
                                 curl);

  return status;
}

HTTPCode HttpConnection::send_post(const std::string& path,
                                   std::map<std::string, std::string>& headers,
                                   const std::string& body,
                                   SAS::TrailId trail,
                                   const std::string& username)
{
  std::string unused_response;
  return HttpConnection::send_post(path, headers, unused_response, body, trail, username);
}

HTTPCode HttpConnection::send_post(const std::string& path,                     //< Absolute path to request from server - must start with "/"
                                   std::map<std::string, std::string>& headers, //< Map of headers from the response
                                   std::string& response,                       //< Retrieved document
                                   const std::string& body,                     //< Body to send in request
                                   SAS::TrailId trail,                          //< SAS trail
                                   const std::string& username)                 //< Username to assert (if assertUser was true, else ignored).
{
  CURL *curl = get_curl_handle();
  curl_easy_setopt(curl, CURLOPT_POST, 1);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, &HttpConnection::write_headers);
  curl_easy_setopt(curl, CURLOPT_WRITEHEADER, &headers);

  std::vector<std::string> unused_extra_headers;
  HTTPCode status = send_request(path, body, response, username, trail, "POST", unused_extra_headers, curl);

  return status;
}

/// Get data; return a HTTP return code
HTTPCode HttpConnection::send_get(const std::string& path,
                                  std::string& response,
                                  const std::string& username,
                                  SAS::TrailId trail)
{
  std::map<std::string, std::string> unused_rsp_headers;
  std::vector<std::string> unused_req_headers;
  return HttpConnection::send_get(path, unused_rsp_headers, response, username, unused_req_headers, trail);
}

/// Get data; return a HTTP return code
HTTPCode HttpConnection::send_get(const std::string& path,
                                  std::string& response,
                                  std::vector<std::string> headers,
                                  const std::string& override_server,
                                  SAS::TrailId trail)
{
  change_server(override_server);

  std::map<std::string, std::string> unused_rsp_headers;
  return HttpConnection::send_get(path, unused_rsp_headers, response, "", headers, trail);
}

/// Get data; return a HTTP return code
HTTPCode HttpConnection::send_get(const std::string& path,
                                  std::map<std::string, std::string>& headers,
                                  std::string& response,
                                  const std::string& username,
                                  SAS::TrailId trail)
{
  std::vector<std::string> unused_req_headers;
  return HttpConnection::send_get(path, headers, response, username, unused_req_headers, trail);
}

/// Get data; return a HTTP return code
HTTPCode HttpConnection::send_get(const std::string& path,                     //< Absolute path to request from server - must start with "/"
                                  std::map<std::string, std::string>& headers, //< Map of headers from the response
                                  std::string& response,                       //< Retrieved document
                                  const std::string& username,                 //< Username to assert (if assertUser was true, else ignored)
                                  std::vector<std::string> headers_to_add,     //< Extra headers to add to the request
                                  SAS::TrailId trail)                          //< SAS trail
{
  CURL *curl = get_curl_handle();
  curl_easy_setopt(curl, CURLOPT_HTTPGET, 1);

  return send_request(path, "", response, username, trail, "GET", headers_to_add, curl);
}

/// Get data; return a HTTP return code
HTTPCode HttpConnection::send_request(const std::string& path,                 //< Absolute path to request from server - must start with "/"
                                      std::string body,                        //< Body to send on the request
                                      std::string& doc,                        //< OUT: Retrieved document
                                      const std::string& username,             //< Username to assert (if assertUser was true, else ignored).
                                      SAS::TrailId trail,                      //< SAS trail to use
                                      const std::string& method_str,           // The method, used for logging.
                                      std::vector<std::string> headers_to_add, //< Extra headers to add to the request
                                      CURL* curl)
{
  std::string url = _scheme + "://" + _server + path;
  struct curl_slist *extra_headers = NULL;
  PoolEntry* entry;
  CURLcode rc = curl_easy_getinfo(curl, CURLINFO_PRIVATE, (char**)&entry);
  assert(rc == CURLE_OK);

  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &doc);

  if (!body.empty())
  {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    extra_headers = curl_slist_append(extra_headers, "Content-Type: application/json");
  }

  // Create a UUID to use for SAS correlation and add it to the HTTP message.
  boost::uuids::uuid uuid = get_random_uuid();
  std::string uuid_str = boost::uuids::to_string(uuid);
  extra_headers = curl_slist_append(extra_headers,
                                    (SASEvent::HTTP_BRANCH_HEADER_NAME + ": " + uuid_str).c_str());

  // Now log the marker to SAS. Flag that SAS should not reactivate the trail
  // group as a result of associations on this marker (doing so after the call
  // ends means it will take a long time to be searchable in SAS).
  SAS::Marker corr_marker(trail, MARKER_ID_VIA_BRANCH_PARAM, 0);
  corr_marker.add_var_param(uuid_str);
  SAS::report_marker(corr_marker, SAS::Marker::Scope::Trace, false);

  // By default cURL will add `Expect: 100-continue` to certain requests. This
  // causes the HTTP stack to send 100 Continue responses, which messes up the
  // SAS call flow. To prevent this add an empty Expect header, which stops
  // cURL from adding its own.
  extra_headers = curl_slist_append(extra_headers, "Expect:");

  // Add in any extra headers
  for (std::vector<std::string>::const_iterator i = headers_to_add.begin();
                                                i != headers_to_add.end();
                                                ++i)
  {
    extra_headers = curl_slist_append(extra_headers, (*i).c_str());
  }

  // Add the user's identity (if required).
  if (_assert_user)
  {
    extra_headers = curl_slist_append(extra_headers,
                                      ("X-XCAP-Asserted-Identity: " + username).c_str());
  }
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, extra_headers);

  // Determine whether to recycle the connection, based on
  // previously-calculated deadline.
  struct timespec tp;
  int rv = clock_gettime(CLOCK_MONOTONIC, &tp);
  assert(rv == 0);
  unsigned long now_ms = tp.tv_sec * 1000 + (tp.tv_nsec / 1000000);
  bool recycle_conn = entry->is_connection_expired(now_ms);

  // Resolve the host.
  std::vector<AddrInfo> targets;
  _resolver->resolve(_host, _port, MAX_TARGETS, targets, trail);

  // If we're not recycling the connection, try to get the current connection
  // IP address and add it to the front of the target list (if it was there)
  if (!recycle_conn)
  {
    char* primary_ip;
    if (curl_easy_getinfo(curl, CURLINFO_PRIMARY_IP, &primary_ip) == CURLE_OK)
    {
      AddrInfo ai;
      _resolver->parse_ip_target(primary_ip, ai.address);
      ai.port = (_port != 0) ? _port : 80;
      ai.transport = IPPROTO_TCP;

      int initialSize = targets.size();
      targets.erase(std::remove(targets.begin(), targets.end(), ai), targets.end());
      if ((int)targets.size() < initialSize)
      {
        targets.insert(targets.begin(), ai);
      }
    }
  }

  // If the list of targets only contains 1 target, clone it - we always want
  // to retry at least once.
  if (targets.size() == 1)
  {
    targets.push_back(targets[0]);
  }

  // Track the number of HTTP 503 and 504 responses and the number of timeouts
  // or I/O errors.
  int num_http_503_responses = 0;
  int num_http_504_responses = 0;
  int num_timeouts_or_io_errors = 0;

  // Track the IP addresses we're connecting to.  If we fail, we failed to
  // resolve the host, so default to that.
  const char *remote_ip = NULL;
  rc = CURLE_COULDNT_RESOLVE_HOST;

  // Try to get a decent connection - try each of the hosts in turn (although
  // we might quit early if we have too many HTTP-level failures).
  for (std::vector<AddrInfo>::const_iterator i = targets.begin();
       i != targets.end();
       ++i)
  {
    TRC_DEBUG("recycle_conn: %d", recycle_conn);
    curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, recycle_conn ? 1L : 0L);

    char buf[100];
    remote_ip = inet_ntop(i->address.af, &i->address.addr, buf, sizeof(buf));

    // Each time round this loop we add an entry to curl's DNS cache, and also
    // leave a note reminding ourselves to remove the entry next time.
    //
    // Retrieve that note (which may not be present).
    curl_slist *host_resolve = entry->get_host_resolve();
    entry->set_host_resolve(NULL);

    // Add the new entry.
    std::string resolve_addr = _host + ":" + std::to_string(i->port) + ":" + remote_ip;
    host_resolve = curl_slist_append(host_resolve, resolve_addr.c_str());
     TRC_DEBUG("Set CURLOPT_RESOLVE: %s", resolve_addr.c_str());
    curl_easy_setopt(curl, CURLOPT_RESOLVE, host_resolve);

    // Set the curl target URL
    std::string ip_url = _scheme + "://" + _host + ":" + std::to_string(i->port) + path;
    TRC_DEBUG("Set CURLOPT_URL: %s", ip_url.c_str());
    curl_easy_setopt(curl, CURLOPT_URL, ip_url.c_str());

    // Create and register an object to record the HTTP transaction.
    Recorder recorder;
    curl_easy_setopt(curl, CURLOPT_DEBUGDATA, &recorder);

    // Get the current timestamp before calling into curl.  This is because we
    // can't log the request to SAS until after curl_easy_perform has returned.
    // This could be a long time if the server is being slow, and we want to log
    // the request with the right timestamp.
    SAS::Timestamp req_timestamp = SAS::get_current_timestamp();

    // Send the request.
    doc.clear();
    TRC_DEBUG("Sending HTTP request : %s (trying %s) %s", url.c_str(), remote_ip, (recycle_conn) ? "on new connection" : "");
    rc = curl_easy_perform(curl);

    // Leave ourselves a note to remove the DNS entry from curl's cache next time round.
    curl_slist_free_all(host_resolve);
    std::string resolve_remove_addr = std::string("-") + _host + ":" + std::to_string(i->port);
    host_resolve = curl_slist_append(NULL, resolve_remove_addr.c_str());
    entry->set_host_resolve(host_resolve);

    // If a request was sent, log it to SAS.
    if (recorder.request.length() > 0)
    {
      sas_log_http_req(trail, curl, method_str, url, recorder.request, req_timestamp, 0);
    }

    // Log the result of the request.
    long http_rc = 0;
    if (rc == CURLE_OK)
    {
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_rc);
      sas_log_http_rsp(trail, curl, http_rc, method_str, url, recorder.response, 0);
      TRC_DEBUG("Received HTTP response: status=%d, doc=%s", http_rc, doc.c_str());
    }
    else
    {
      TRC_ERROR("%s failed at server %s : %s (%d) : fatal",
                url.c_str(), remote_ip, curl_easy_strerror(rc), rc);
      sas_log_curl_error(trail, remote_ip, i->port, method_str, url, rc, 0);
    }

    // Update the connection recycling and retry algorithms.
    if ((rc == CURLE_OK) && !(http_rc >= 400))
    {
      if (recycle_conn)
      {
        entry->update_deadline(now_ms);
      }

      // Success!
      break;
    }
    else
    {
      // If we forced a new connection and we failed even to establish an HTTP
      // connection, blacklist this IP address.
      if (recycle_conn &&
          !(http_rc >= 400) &&
          (rc != CURLE_REMOTE_FILE_NOT_FOUND) &&
          (rc != CURLE_REMOTE_ACCESS_DENIED))
      {
        _resolver->blacklist(*i);
      }

      // Determine the failure mode and update the correct counter.
      bool fatal_http_error = false;
      if (http_rc >= 400)
      {
        if (http_rc == 503)
        {
          num_http_503_responses++;
        }
        // LCOV_EXCL_START fakecurl doesn't let us return custom return codes.
        else if (http_rc == 504)
        {
          num_http_504_responses++;
        }
        else
        {
          fatal_http_error = true;
        }
        // LCOV_EXCL_STOP
      }
      else if ((rc == CURLE_REMOTE_FILE_NOT_FOUND) ||
               (rc == CURLE_REMOTE_ACCESS_DENIED))
      {
        fatal_http_error = true;
      }
      else if ((rc == CURLE_OPERATION_TIMEDOUT) ||
               (rc == CURLE_SEND_ERROR) ||
               (rc == CURLE_RECV_ERROR))
      {
        num_timeouts_or_io_errors++;
      }

      // Decide whether to keep trying.
      if ((num_http_503_responses + num_timeouts_or_io_errors >= 2) ||
          (num_http_504_responses >= 1) ||
          fatal_http_error)
      {
        // Make a SAS log so that its clear that we have stopped retrying
        // deliberately.
        HttpErrorResponseTypes reason = fatal_http_error ?
                                        HttpErrorResponseTypes::Permanent :
                                        HttpErrorResponseTypes::Temporary;
        sas_log_http_abort(trail, reason, 0);
        break;
      }
    }
  }

  // Check whether we should apply a penalty. We do this when:
  //  - both attempts return 503 errors, which means the downstream node is
  //    overloaded/requests to it are timeing.
  //  - the error is a 504, which means that the node downsteam of the node
  //    we're connecting to currently has reported that it is overloaded/was
  //    unresponsive.
  if (((num_http_503_responses >= 2) ||
       (num_http_504_responses >= 1)) &&
      (_load_monitor != NULL))
  {
    _load_monitor->incr_penalties();
  }

  if (rc == CURLE_OK)
  {
    entry->set_remote_ip(remote_ip);

    if (_comm_monitor)
    {
      // If both attempts fail due to overloaded downstream nodes, consider
      // it a communication failure.
      if (num_http_503_responses >= 2)
      {
        _comm_monitor->inform_failure(now_ms); // LCOV_EXCL_LINE - No UT for 503 fails
      }
      else
      {
        _comm_monitor->inform_success(now_ms);
      }
    }
  }
  else
  {
    entry->set_remote_ip("");

    if (_comm_monitor)
    {
      _comm_monitor->inform_failure(now_ms);
    }
  }

  HTTPCode http_code = curl_code_to_http_code(curl, rc);

  if (((rc != CURLE_OK) && (rc != CURLE_REMOTE_FILE_NOT_FOUND)) || (http_code >= 400))
  {
    TRC_ERROR("cURL failure with cURL error code %d (see man 3 libcurl-errors) and HTTP error code %ld", (int)rc, http_code);  // LCOV_EXCL_LINE
  }

  reset_curl_handle(curl);
  curl_slist_free_all(extra_headers);
  return http_code;
}

/// cURL helper - write data into string.
size_t HttpConnection::string_store(void* ptr, size_t size, size_t nmemb, void* stream)
{
  ((std::string*)stream)->append((char*)ptr, size * nmemb);
  return (size * nmemb);
}


/// Called to clean up the cURL handle.
void HttpConnection::cleanup_curl(void* curlptr)
{
  CURL* curl = (CURL*)curlptr;

  PoolEntry* entry;
  CURLcode rc = curl_easy_getinfo(curl, CURLINFO_PRIVATE, (char**)&entry);
  if (rc == CURLE_OK)
  {
    // Connection has closed.
    entry->set_remote_ip("");
    delete entry;
  }

  curl_easy_cleanup(curl);
}


/// PoolEntry constructor
HttpConnection::PoolEntry::PoolEntry(HttpConnection* parent) :
  _parent(parent),
  _deadline_ms(0L),
  _rand(1.0 / CONNECTION_AGE_MS),
  _host_resolve(NULL)
{
}


/// PoolEntry destructor
HttpConnection::PoolEntry::~PoolEntry()
{
  if (_host_resolve != NULL)
  {
    curl_slist_free_all(_host_resolve);
    _host_resolve = NULL;
  }
}


/// Is it time to recycle the connection? Expects CLOCK_MONOTONIC
/// current time, in milliseconds.
bool HttpConnection::PoolEntry::is_connection_expired(unsigned long now_ms)
{
  return (now_ms > _deadline_ms);
}


/// Update deadline to next appropriate value. Expects
/// CLOCK_MONOTONIC current time, in milliseconds.  Call on
/// successful connection.
void HttpConnection::PoolEntry::update_deadline(unsigned long now_ms)
{
  // Get the next desired inter-arrival time. Take a random sample
  // from an exponential distribution so as to avoid spikes.
  unsigned long interval_ms = (unsigned long)_rand();

  if ((_deadline_ms == 0L) ||
      ((_deadline_ms + interval_ms) < now_ms))
  {
    // This is the first request, or the new arrival time has
    // already passed (in which case things must be pretty
    // quiet). Just bump the next deadline into the future.
    _deadline_ms = now_ms + interval_ms;
  }
  else
  {
    // The new arrival time is yet to come. Schedule it relative to
    // the last intended time, so as not to skew the mean
    // upwards.

    // We don't recycle any connections in the UTs. (We could do this
    // by manipulating time, but would have no way of checking it
    // worked.)
    _deadline_ms += interval_ms; // LCOV_EXCL_LINE
  }
}


/// Set the remote IP, and update statistics.
void HttpConnection::PoolEntry::set_remote_ip(const std::string& value)  //< Remote IP, or "" if no connection.
{
  if (value == _remote_ip)
  {
    return;
  }


  if (_parent->_stat_table != NULL)
  {
    update_snmp_ip_counts(value);
  }

  _remote_ip = value;
}

void HttpConnection::PoolEntry::update_snmp_ip_counts(const std::string& value)  //< Remote IP, or "" if no connection.
{
  pthread_mutex_lock(&_parent->_lock);

  if (!_remote_ip.empty())
  {
      if (_parent->_stat_table->get(_remote_ip)->decrement() == 0)
      {
        _parent->_stat_table->remove(_remote_ip);
      }
  }

  if (!value.empty())
  {
      _parent->_stat_table->get(value)->increment();
  }

  pthread_mutex_unlock(&_parent->_lock);
}

size_t HttpConnection::write_headers(void *ptr, size_t size, size_t nmemb, std::map<std::string, std::string> *headers)
{
  char* headerLine = reinterpret_cast<char *>(ptr);

  // convert to string
  std::string headerString(headerLine, (size * nmemb));

  std::string key;
  std::string val;

  // find colon
  size_t colon_loc = headerString.find(":");
  if (colon_loc == std::string::npos)
  {
    key = headerString;
    val = "";
  }
  else
  {
    key = headerString.substr(0, colon_loc);
    val = headerString.substr(colon_loc + 1, std::string::npos);
  }

  // Lowercase the key (for consistency) and remove spaces
  std::transform(key.begin(), key.end(), key.begin(), ::tolower);
  key.erase(std::remove_if(key.begin(), key.end(), ::isspace), key.end());
  val.erase(std::remove_if(val.begin(), val.end(), ::isspace), val.end());

  TRC_DEBUG("Received header %s with value %s", key.c_str(), val.c_str());
  (*headers)[key] = val;

  return size * nmemb;
}

void HttpConnection::cleanup_uuid(void *uuid_gen)
{
  delete (RandomUUIDGenerator*)uuid_gen; uuid_gen = NULL;
}

boost::uuids::uuid HttpConnection::get_random_uuid()
{
  // Get the factory from thread local data (creating it if it doesn't exist).
  RandomUUIDGenerator* uuid_gen;
  uuid_gen =
    (RandomUUIDGenerator*)pthread_getspecific(_uuid_thread_local);

  if (uuid_gen == NULL)
  {
    uuid_gen = new RandomUUIDGenerator();
    pthread_setspecific(_uuid_thread_local, uuid_gen);
  }

  // _uuid_gen_ is a pointer to a callable object that returns a UUID.
  return (*uuid_gen)();
}

void HttpConnection::sas_add_ip(SAS::Event& event, CURL* curl, CURLINFO info)
{
  char* ip;

  if (curl_easy_getinfo(curl, info, &ip) == CURLE_OK)
  {
    event.add_var_param(ip);
  }
  else
  {
    event.add_var_param("unknown"); // LCOV_EXCL_LINE FakeCurl cannot fail the getinfo call.
  }
}

void HttpConnection::sas_add_port(SAS::Event& event, CURL* curl, CURLINFO info)
{
  long port;

  if (curl_easy_getinfo(curl, info, &port) == CURLE_OK)
  {
    event.add_static_param(port);
  }
  else
  {
    event.add_static_param(0); // LCOV_EXCL_LINE FakeCurl cannot fail the getinfo call.
  }
}

void HttpConnection::sas_add_ip_addrs_and_ports(SAS::Event& event,
                                                CURL* curl)
{
  // Add the local IP and port.
  sas_add_ip(event, curl, CURLINFO_PRIMARY_IP);
  sas_add_port(event, curl, CURLINFO_PRIMARY_PORT);

  // Now add the remote IP and port.
  sas_add_ip(event, curl, CURLINFO_LOCAL_IP);
  sas_add_port(event, curl, CURLINFO_LOCAL_PORT);
}

void HttpConnection::sas_log_http_req(SAS::TrailId trail,
                                      CURL* curl,
                                      const std::string& method_str,
                                      const std::string& url,
                                      const std::string& request_bytes,
                                      SAS::Timestamp timestamp,
                                      uint32_t instance_id)
{
  if (_sas_log_level != SASEvent::HttpLogLevel::NONE)
  {
    int event_id = ((_sas_log_level == SASEvent::HttpLogLevel::PROTOCOL) ?
                    SASEvent::TX_HTTP_REQ : SASEvent::TX_HTTP_REQ_DETAIL);
    SAS::Event event(trail, event_id, instance_id);

    sas_add_ip_addrs_and_ports(event, curl);
    event.add_compressed_param(request_bytes, &SASEvent::PROFILE_HTTP);
    event.add_var_param(method_str);
    event.add_var_param(Utils::url_unescape(url));

    event.set_timestamp(timestamp);
    SAS::report_event(event);
  }
}

void HttpConnection::sas_log_http_rsp(SAS::TrailId trail,
                                      CURL* curl,
                                      long http_rc,
                                      const std::string& method_str,
                                      const std::string& url,
                                      const std::string& response_bytes,
                                      uint32_t instance_id)
{
  if (_sas_log_level != SASEvent::HttpLogLevel::NONE)
  {
    int event_id = ((_sas_log_level == SASEvent::HttpLogLevel::PROTOCOL) ?
                    SASEvent::RX_HTTP_RSP : SASEvent::RX_HTTP_RSP_DETAIL);
    SAS::Event event(trail, event_id, instance_id);

    sas_add_ip_addrs_and_ports(event, curl);
    event.add_static_param(http_rc);
    event.add_compressed_param(response_bytes, &SASEvent::PROFILE_HTTP);
    event.add_var_param(method_str);
    event.add_var_param(Utils::url_unescape(url));

    SAS::report_event(event);
  }
}

void HttpConnection::sas_log_http_abort(SAS::TrailId trail,
                                        HttpErrorResponseTypes reason,
                                        uint32_t instance_id)
{
  int event_id = ((_sas_log_level == SASEvent::HttpLogLevel::PROTOCOL) ?
                    SASEvent::HTTP_ABORT : SASEvent::HTTP_ABORT_DETAIL);
  SAS::Event event(trail, event_id, instance_id);
  event.add_static_param(static_cast<uint32_t>(reason));
  SAS::report_event(event);
}

void HttpConnection::sas_log_curl_error(SAS::TrailId trail,
                                        const char* remote_ip_addr,
                                        unsigned short remote_port,
                                        const std::string& method_str,
                                        const std::string& url,
                                        CURLcode code,
                                        uint32_t instance_id)
{
  if (_sas_log_level != SASEvent::HttpLogLevel::NONE)
  {
    int event_id = ((_sas_log_level == SASEvent::HttpLogLevel::PROTOCOL) ?
                    SASEvent::HTTP_REQ_ERROR : SASEvent::HTTP_REQ_ERROR_DETAIL);
    SAS::Event event(trail, event_id, instance_id);

    event.add_static_param(remote_port);
    event.add_static_param(code);
    event.add_var_param(remote_ip_addr);
    event.add_var_param(method_str);
    event.add_var_param(Utils::url_unescape(url));
    event.add_var_param(curl_easy_strerror(code));

    SAS::report_event(event);
  }
}

void HttpConnection::host_port_from_server(const std::string& server, std::string& host, int& port)
{
  std::string server_copy = server;
  Utils::trim(server_copy);
  size_t colon_idx;
  if (((server_copy[0] != '[') ||
       (server_copy[server_copy.length() - 1] != ']')) &&
      ((colon_idx = server_copy.find_last_of(':')) != std::string::npos))
  {
    host = server_copy.substr(0, colon_idx);
    port = stoi(server_copy.substr(colon_idx + 1));
  }
  else
  {
    host = server_copy;
    port = 0;
  }
}

std::string HttpConnection::host_from_server(const std::string& server)
{
  std::string host;
  int port;
  host_port_from_server(server, host, port);
  return host;
}

int HttpConnection::port_from_server(const std::string& server)
{
  std::string host;
  int port;
  host_port_from_server(server, host, port);
  return port;
}

// Changes the underlying server used by this connection. Use this when
// the HTTPConnection was created without a server (e.g.
// ChronosInternalConnection)
void HttpConnection::change_server(std::string override_server)
{
  _server = override_server;
  _host = host_from_server(override_server);
  _port = port_from_server(override_server);
}

// This function determines an appropriate absolute HTTP request timeout
// (in ms) given the target latency for requests that the downstream components
// will be using.
long HttpConnection::calc_req_timeout_from_latency(int latency_us)
{
  return std::max(1, (latency_us * TIMEOUT_LATENCY_MULTIPLIER) / 1000);
}

HttpConnection::Recorder::Recorder() {}

HttpConnection::Recorder::~Recorder() {}

int HttpConnection::Recorder::debug_callback(CURL *handle,
                                             curl_infotype type,
                                             char *data,
                                             size_t size,
                                             void *userptr)
{
  return ((Recorder*)userptr)->record_data(type, data, size);
}

int HttpConnection::Recorder::record_data(curl_infotype type,
                                          char* data,
                                          size_t size)
{
  switch (type)
  {
  case CURLINFO_HEADER_IN:
  case CURLINFO_DATA_IN:
    response.append(data, size);
    break;

  case CURLINFO_HEADER_OUT:
  case CURLINFO_DATA_OUT:
    request.append(data, size);
    break;

  default:
    break;
  }

  return 0;
}
