/*
    This file is part of Kismet

    Kismet is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kismet is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kismet; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "config.h"

#include <memory>

#include <stdio.h>
#include <time.h>
#include <list>
#include <map>
#include <vector>

#include "kismet_algorithm.h"

#include <string>
#include <sstream>
#include <pthread.h>

#include "globalregistry.h"
#include "util.h"
#include "configfile.h"
#include "messagebus.h"
#include "packetchain.h"
#include "devicetracker.h"
#include "packet.h"
#include "gpstracker.h"
#include "alertracker.h"
#include "manuf.h"
#include "entrytracker.h"
#include "devicetracker_component.h"
#include "json_adapter.h"
#include "structured.h"
#include "kismet_json.h"
#include "base64.h"

// HTTP interfaces
bool device_tracker::httpd_verify_path(const char *path, const char *method) {
    if (strcmp(method, "GET") == 0) {
        // Simple fixed URLS

        std::string stripped = httpd_strip_suffix(path);

        // Explicit compare for .ekjson because it doesn't serialize the 
        // same way
        if (strcmp(path, "/devices/all_devices.ekjson") == 0)
            return true;

        // Split URL and process
        std::vector<std::string> tokenurl = str_tokenize(path, "/");
        if (tokenurl.size() < 2)
            return false;

        if (tokenurl[1] == "devices") {
            if (tokenurl.size() < 3)
                return false;

            // Do a by-key lookup and return the device or the device path
            if (tokenurl[2] == "by-key") {
                if (tokenurl.size() < 5) {
                    return false;
                }

                device_key key(tokenurl[3]);

                if (key.get_error())
                    return false;

                if (!httpd_can_serialize(tokenurl[4]))
                    return false;

                auto tmi = fetch_device(key);

                if (tmi == NULL)
                    return false;

                std::string target = httpd_strip_suffix(tokenurl[4]);

                if (target == "device") {
                    // Try to find the exact field
                    if (tokenurl.size() > 5) {
                        std::vector<std::string>::const_iterator first = tokenurl.begin() + 5;
                        std::vector<std::string>::const_iterator last = tokenurl.end();
                        std::vector<std::string> fpath(first, last);

                        if (tmi->get_child_path(fpath) == NULL) {
                            return false;
                        }
                    }

                    return true;
                }

                return false;
            } else if (tokenurl[2] == "by-mac") {
                if (tokenurl.size() < 5)
                    return false;

                if (!httpd_can_serialize(tokenurl[4]))
                    return false;

                mac_addr mac = mac_addr(tokenurl[3]);

                if (mac.error) {
                    return false;
                }

                { 
                    local_shared_locker devlock(&devicelist_mutex);

                    if (tracked_mac_multimap.count(mac) > 0)
                        return true;
                }

                return false;
            } else if (tokenurl[2] == "last-time") {
                if (tokenurl.size() < 5) {
                    return false;
                }

                long lastts;
                if (sscanf(tokenurl[3].c_str(), "%ld", &lastts) != 1) {
                    return false;
                }

                // Explicit catch of ekjson
                if (tokenurl[4] == "devices.ekjson")
                    return true;

                return httpd_can_serialize(tokenurl[4]);
            }
        }
    } else if (strcmp(method, "POST") == 0) {
        // Split URL and process
        std::vector<std::string> tokenurl = str_tokenize(path, "/");
        if (tokenurl.size() < 2)
            return false;

        if (tokenurl[1] == "devices") {
            if (tokenurl.size() < 4) {
                return false;
            } else if (tokenurl[2] == "last-time") {
                if (tokenurl.size() < 5) {
                    return false;
                }

                long lastts;
                if (sscanf(tokenurl[3].c_str(), "%ld", &lastts) != 1) {
                    fprintf(stderr, "debug - unable to parse ts\n");
                    return false;
                }

                return httpd_can_serialize(tokenurl[4]);
            } else if (tokenurl[2] == "by-key") {
                if (tokenurl.size() < 5) {
                    return false;
                }

                device_key key(tokenurl[3]);

                if (key.get_error())
                    return false;

                if (!httpd_can_serialize(tokenurl[4]))
                    return false;

                if (fetch_device(key) == NULL)
                    return false;

                std::string target = httpd_strip_suffix(tokenurl[4]);

                if (target == "device") {
                    return true;
                }

                if (target == "set_name") {
                    return true;
                }

                if (target == "set_tag") {
                    return true;
                }
            } else if (tokenurl[2] == "by-mac") {
                if (tokenurl.size() < 5)
                    return false;

                if (!httpd_can_serialize(tokenurl[4]))
                    return false;

                mac_addr mac = mac_addr(tokenurl[3]);

                if (mac.error) {
                    return false;
                }

                {
                    local_shared_locker listlocker(&devicelist_mutex);
                    if (tracked_mac_multimap.count(mac) > 0)
                        return true;
                }

                return false;
            }
        }
    }

    return false;
}

int device_tracker::httpd_create_stream_response(
        kis_net_httpd *httpd __attribute__((unused)),
        kis_net_httpd_connection *connection,
        const char *path, const char *method, const char *upload_data,
        size_t *upload_data_size) {

    // fmt::print(stderr, "createstreamresponse path {}\n", path);

    if (strcmp(method, "GET") != 0) {
        return MHD_YES;
    }

    // Allocate our buffer aux
    kis_net_httpd_buffer_stream_aux *saux = 
        (kis_net_httpd_buffer_stream_aux *) connection->custom_extension;

    buffer_handler_ostringstream_buf *streambuf = 
        new buffer_handler_ostringstream_buf(saux->get_rbhandler());
    std::ostream stream(streambuf);

    // Set our cleanup function
    saux->set_aux(streambuf, 
            [](kis_net_httpd_buffer_stream_aux *aux) {
                if (aux->aux != NULL)
                    delete((buffer_handler_ostringstream_buf *) (aux->aux));
            });

    // Set our sync function which is called by the webserver side before we
    // clean up...
    saux->set_sync([](kis_net_httpd_buffer_stream_aux *aux) {
            if (aux->aux != NULL) {
                ((buffer_handler_ostringstream_buf *) aux->aux)->pubsync();
                }
            });


    if (strcmp(path, "/devices/all_devices.ekjson") == 0) {
        // Instantiate a manual serializer
        json_adapter::serializer serial; 

        auto fw = std::make_shared<devicetracker_function_worker>(
                [&stream, &serial](device_tracker *, std::shared_ptr<kis_tracked_device_base> d) -> bool {
                    serial.serialize(d, stream);
                    stream << "\n";

                    // Return false because we're not building a list, we're serializing
                    // per element
                    return false;
                }, nullptr);

        do_readonly_device_work(fw);
        return MHD_YES;
    }

    std::string stripped = httpd_strip_suffix(path);

    // fmt::print(stderr, "tokenizing path {}\n", path);

    std::vector<std::string> tokenurl = str_tokenize(path, "/");

    // fmt::print(stderr, "path {} tokenized to size {}\n", path, tokenurl.size());

    if (tokenurl.size() < 2) {
        return MHD_YES;
    }

    if (tokenurl[1] == "devices") {
        if (tokenurl.size() < 5)
            return MHD_YES;

        if (tokenurl[2] == "by-key") {
            if (tokenurl.size() < 5) {
                _MSG_ERROR("HTTP request for {}; invalid by-key URI", path);
                stream << "Invalid by-key URI\n";
                connection->httpcode = 500;
                return MHD_YES;
            }

            if (!httpd_can_serialize(tokenurl[4])) {
                _MSG_ERROR("HTTP request for {}; can't actually serialize.", path);
                connection->httpcode = 500;
                return MHD_YES;
            }

            device_key key(tokenurl[3]);
            auto dev = fetch_device(key);

            if (dev == nullptr) {
                _MSG_ERROR("HTTP request for {}; invalid device key {}", path, tokenurl[3]);
                stream << "Invalid device key\n";
                connection->httpcode = 500;
                return MHD_YES;
            }

            std::string target = httpd_strip_suffix(tokenurl[4]);

            if (target == "device") {
                // Try to find the exact field
                if (tokenurl.size() > 5) {
                    std::vector<std::string>::const_iterator first = tokenurl.begin() + 5;
                    std::vector<std::string>::const_iterator last = tokenurl.end();
                    std::vector<std::string> fpath(first, last);

                    local_shared_locker devlocker(&(dev->device_mutex));

                    shared_tracker_element sub = dev->get_child_path(fpath);

                    if (sub == nullptr) {
                        _MSG_ERROR("HTTP request for {}; could not map child path to a device record node.", path);
                        stream << "Invalid sub-key path\n";
                        connection->httpcode = 500;
                        return MHD_YES;
                    } 

                    // Set the mime component of the url
                    connection->mime_url = tokenurl[4];

                    Globalreg::globalreg->entrytracker->serialize(httpd->get_suffix(tokenurl[4]), stream, sub, NULL);
                    return MHD_YES;
                }

                Globalreg::globalreg->entrytracker->serialize(httpd->get_suffix(tokenurl[4]), stream, dev, NULL);
                // fmt::print(stderr, "Wrote data for key {}", key);

                return MHD_YES;
            } else {
                stream << "<h1>Server error</h1>Unhandled by-key target.";
                connection->httpcode = 500;
                return MHD_YES;
            }
        } else if (tokenurl[2] == "by-mac") {
            if (tokenurl.size() < 5)
                return MHD_YES;

            if (!httpd_can_serialize(tokenurl[4]))
                return MHD_YES;

            local_shared_locker lock(&devicelist_mutex);

            mac_addr mac = mac_addr(tokenurl[3]);

            if (mac.error) {
                return MHD_YES;
            }

            auto devvec = std::make_shared<tracker_element_vector>();

            const auto& mmp = tracked_mac_multimap.equal_range(mac);
            for (auto mmpi = mmp.first; mmpi != mmp.second; ++mmpi) {
                devvec->push_back(mmpi->second);
            }

            Globalreg::globalreg->entrytracker->serialize(httpd->get_suffix(tokenurl[4]), stream, devvec, NULL);

            return MHD_YES;
        } else if (tokenurl[2] == "last-time") {
            if (tokenurl.size() < 5)
                return MHD_YES;

            // Is the timestamp an int?
            long lastts;
            if (sscanf(tokenurl[3].c_str(), "%ld", &lastts) != 1)
                return MHD_YES;

            // If it's negative, subtract from the current ts
            if (lastts < 0) {
                time_t now = time(0);
                lastts = now + lastts;
            }

            if (!httpd_can_serialize(tokenurl[4]))
                return MHD_YES;

            std::shared_ptr<tracker_element_vector> devvec;

            auto fw = std::make_shared<devicetracker_function_worker>(
                    [devvec, lastts](device_tracker *, 
                        std::shared_ptr<kis_tracked_device_base> d) -> bool {
                        if (d->get_last_time() <= lastts)
                            return false;

                        return true;
                    }, nullptr);
            do_readonly_device_work(fw);
            devvec = fw->GetMatchedDevices();

            Globalreg::globalreg->entrytracker->serialize(httpd->get_suffix(tokenurl[4]), stream, devvec, NULL);

            return MHD_YES;
        }

    }

    return MHD_YES;
}

int device_tracker::httpd_post_complete(kis_net_httpd_connection *concls) {
    // Split URL and process
    std::vector<std::string> tokenurl = str_tokenize(concls->url, "/");

    auto saux = (kis_net_httpd_buffer_stream_aux *) concls->custom_extension;
    auto streambuf = new buffer_handler_ostringstream_buf(saux->get_rbhandler());

    std::ostream stream(streambuf);

    saux->set_aux(streambuf, 
            [](kis_net_httpd_buffer_stream_aux *aux) {
                if (aux->aux != NULL)
                    delete((buffer_handler_ostringstream_buf *) (aux->aux));
            });

    // Set our sync function which is called by the webserver side before we
    // clean up...
    saux->set_sync([](kis_net_httpd_buffer_stream_aux *aux) {
            if (aux->aux != NULL) {
                ((buffer_handler_ostringstream_buf *) aux->aux)->pubsync();
                }
            });

    // All URLs are at least /devices/by-foo/y/x
    if (tokenurl.size() < 4) {
        stream << "Invalid request";
        concls->httpcode = 400;
        return MHD_YES;
    }

    // Common structured API data
    shared_structured structdata;

    // Summarization vector
    std::vector<SharedElementSummary> summary_vec;

    // Wrapper, if any
    std::string wrapper_name;

    // Rename cache generated during simplification
    auto rename_map = std::make_shared<tracker_element_serializer::rename_map>();

    shared_structured regexdata;

    time_t post_ts = 0;

    try {
        if (concls->variable_cache.find("json") != 
                concls->variable_cache.end()) {
            structdata =
                std::make_shared<structured_json>(concls->variable_cache["json"]->str());
        } else {
            // fprintf(stderr, "debug - missing data\n");
            throw structured_data_exception("Missing data; expected command dictionary in json= field");
        }
    } catch(const structured_data_exception& e) {
        stream << "Invalid request: ";
        stream << e.what();
        concls->httpcode = 400;
        return MHD_YES;
    }

    try {
        if (structdata->has_key("fields")) {
            shared_structured fields = structdata->get_structured_by_key("fields");
            structured_data::structured_vec fvec = fields->as_vector();

            for (const auto& i : fvec) {
                if (i->is_string()) {
                    auto s = std::make_shared<tracker_element_summary>(i->as_string());
                    summary_vec.push_back(s);
                } else if (i->is_array()) {
                    structured_data::string_vec mapvec = i->as_string_vector();

                    if (mapvec.size() != 2) {
                        // fprintf(stderr, "debug - malformed rename pair\n");
                        stream << "Invalid request: Expected field, rename";
                        concls->httpcode = 400;
                        return MHD_YES;
                    }

                    auto s = 
                        std::make_shared<tracker_element_summary>(mapvec[0], mapvec[1]);
                    summary_vec.push_back(s);
                }
            }
        }

        // Get the wrapper, if one exists, default to empty if it doesn't
        wrapper_name = structdata->key_as_string("wrapper", "");

        if (structdata->has_key("regex")) {
            regexdata = structdata->get_structured_by_key("regex");
        }

        if (structdata->has_key("last_time")) {
            int64_t rawt = structdata->key_as_number("last_time");

            if (rawt < 0)
                post_ts = time(0) + rawt;
            else
                post_ts = rawt;
        }
    } catch(const structured_data_exception& e) {
        stream << "Invalid request: Malformed command dictionary, ";
        stream << e.what();
        concls->httpcode = 400;
        return MHD_YES;
    }

    try {
        if (tokenurl[1] == "devices") {
            if (tokenurl[2] == "by-mac") {
                if (tokenurl.size() < 5) {
                    stream << "Invalid request: Invalid URI\n";
                    concls->httpcode = 400;
                    return MHD_YES;
                }

                local_demand_locker lock(&devicelist_mutex);

                if (!httpd_can_serialize(tokenurl[4])) {
                    stream << "Invalid request: Cannot find serializer for file type\n";
                    concls->httpcode = 400;
                    return MHD_YES;
                }

                mac_addr mac = mac_addr(tokenurl[3]);

                
                if (mac.error) {
                    stream << "Invalid request: Invalid MAC address\n";
                    concls->httpcode = 400;
                    return MHD_YES;
                }

                lock.lock();
                if (tracked_mac_multimap.count(mac) == 0) {
                    stream << "Invalid request: Could not find device by MAC\n";
                    concls->httpcode = 400;
                    return MHD_YES;
                }
                lock.unlock();

                std::string target = httpd_strip_suffix(tokenurl[4]);

                if (target == "devices") {
                    auto devvec = std::make_shared<tracker_element_vector>();

                    lock.lock();
                    auto mmp = tracked_mac_multimap.equal_range(mac);
                    lock.unlock();

                    for (auto mmpi = mmp.first; mmpi != mmp.second; ++mmpi) 
                        devvec->push_back(SummarizeSingletracker_element(mmpi->second, summary_vec, rename_map));

                    Globalreg::globalreg->entrytracker->serialize(httpd->get_suffix(tokenurl[4]), stream, 
                            devvec, rename_map);

                    return MHD_YES;
                }

                stream << "Invalid request";
                concls->httpcode = 400;
                return MHD_YES;
            } else if (tokenurl[2] == "by-key") {
                if (tokenurl.size() < 5) {
                    stream << "Invalid request: Invalid URI";
                    concls->httpcode = 400;
                    return MHD_YES;
                }

                if (!httpd_can_serialize(tokenurl[4])) {
                    stream << "Invalid request: Cannot serialize field type";
                    concls->httpcode = 400;
                    return MHD_YES;
                }

                device_key key(tokenurl[3]);

                auto dev = fetch_device(key);

                if (dev == NULL) {
                    stream << "Invalid request: No device with that key";
                    concls->httpcode = 400;
                    return MHD_YES;
                }

                std::string target = httpd_strip_suffix(tokenurl[4]);

                if (target == "device") {
                    local_shared_locker devlock(&(dev->device_mutex));

                    auto simple = 
                        SummarizeSingletracker_element(dev, summary_vec, rename_map);

                    Globalreg::globalreg->entrytracker->serialize(httpd->get_suffix(tokenurl[4]), 
                            stream, simple, rename_map);

                    return MHD_YES;
                }

                if (target == "set_name") {
                    std::string name;

                    // Must have a session to set the name
                    if (!httpd->has_valid_session(concls)) 
                        throw std::runtime_error("login required");

                    if (!structdata->has_key("username")) 
                        throw std::runtime_error("expected username in command dictionary");

                    name = structdata->key_as_string("username");

                    set_device_user_name(dev, name);

                    stream << "OK";
                    return MHD_YES;
                }

                if (target == "set_tag") {
                    std::string tag, content;

                    if (!httpd->has_valid_session(concls))
                        throw std::runtime_error("login required");

                    if (!structdata->has_key("tagname"))
                        throw std::runtime_error("expected tagname in command dictionary");

                    if (!structdata->has_key("tagvalue"))
                        throw std::runtime_error("expected tagvalue in command dictionary");

                    tag = structdata->key_as_string("tagname");
                    content = structdata->key_as_string("tagvalue");

                    set_device_tag(dev, tag, content);

                    stream << "OK";
                    return MHD_YES;
                }

            } else if (tokenurl[2] == "last-time") {
                // We don't lock the device list since we use workers

                if (tokenurl.size() < 5) {
                    stream << "Invalid request";
                    concls->httpcode = 400;
                    return MHD_YES;
                }

                // Is the timestamp an int?
                long lastts;
                if (sscanf(tokenurl[3].c_str(), "%ld", &lastts) != 1 ||
                        !httpd_can_serialize(tokenurl[4])) {
                    stream << "Invalid request";
                    concls->httpcode = 400;
                    return MHD_YES;
                }

                // If it's negative, subtract from the current ts
                if (lastts < 0) {
                    time_t now = time(0);
                    lastts = now + lastts;
                }

                // Rename cache generated during simplification
                auto rename_map = std::make_shared<tracker_element_serializer::rename_map>();

                // List of devices that pass the timestamp filter
                std::shared_ptr<tracker_element_vector> timedevs;

                //  List of devices that pass the regex filter
                auto regexdevs = std::make_shared<tracker_element_vector>();

                auto tw = std::make_shared<devicetracker_function_worker>(
                        [lastts](device_tracker *, std::shared_ptr<kis_tracked_device_base> d) -> bool {

                        if (d->get_last_time() <= lastts)
                            return false;

                        return true;
                        }, nullptr);
                do_readonly_device_work(tw);
                timedevs = tw->GetMatchedDevices();

                if (regexdata != NULL) {
                    auto worker = std::make_shared<devicetracker_pcre_worker>(regexdata);
                    do_readonly_device_work(worker, timedevs);
                    regexdevs = worker->GetMatchedDevices();
                } else {
                    regexdevs = timedevs;
                }

                // Final devices being simplified and sent out
                auto outdevs = std::make_shared<tracker_element_vector>();

                for (const auto& rei : *regexdevs) {
                    auto rd = std::static_pointer_cast<kis_tracked_device_base>(rei);
                    local_shared_locker lock(&rd->device_mutex);

                    outdevs->push_back(SummarizeSingletracker_element(rd, summary_vec, rename_map));
                }

                Globalreg::globalreg->entrytracker->serialize(httpd->get_suffix(tokenurl[4]), stream, 
                        outdevs, rename_map);
                return MHD_YES;
            }
        }
    } catch(const std::exception& e) {
        stream << "Invalid request: ";
        stream << e.what();
        concls->httpcode = 400;
        return MHD_YES;
    }

    stream << "OK";

    return MHD_YES;
}

unsigned int device_tracker::multimac_endp_handler(std::ostream& stream, const std::string& uri,
        shared_structured structured, kis_net_httpd_connection::variable_cache_map& variable_cache) {

    try {
        auto ret_devices = std::make_shared<tracker_element_vector>();
        auto macs = std::vector<mac_addr>{};

        if (!structured->has_key("devices"))
            throw std::runtime_error("Missing 'devices' key in command dictionary");
        
        auto maclist = structured->get_structured_by_key("devices")->as_vector();

        for (auto m : maclist) {
            mac_addr ma{m->as_string()};

            if (ma.error) 
                throw std::runtime_error(fmt::format("Invalid MAC address '{}' in 'devices' list",
                            kishttpd::escape_html(m->as_string())));

            macs.push_back(ma);
        }

        // Duplicate the mac index so that we're 'immune' to things changing it under us; because we
        // may have quite a number of devices in our query list, this is safest.
        local_demand_locker l(&devicelist_mutex);
        l.lock();
        auto immutable_copy = 
            std::multimap<mac_addr, std::shared_ptr<kis_tracked_device_base>>{tracked_mac_multimap};
        l.unlock();

        // Pull all the devices out of the list
        for (auto m : macs) {
            const auto& mi = immutable_copy.equal_range(m);
            for (auto msi = mi.first; msi != mi.second; ++msi)
                ret_devices->push_back(msi->second);
        }

        // Summarize it all at once
        auto rename_map = std::make_shared<tracker_element_serializer::rename_map>();

        auto output = 
            kishttpd::summarize_with_structured(ret_devices, structured, rename_map);

        Globalreg::globalreg->entrytracker->serialize(kishttpd::get_suffix(uri), stream, output, rename_map);

        return 200;

    } catch (const std::exception& e) {
        stream << "Invalid request: " << e.what() << "\n";
        return 500;
    }

    stream << "Unhandled request\n";
    return 500;
}

std::shared_ptr<tracker_element> device_tracker::all_phys_endp_handler() {
    auto ret_vec = 
        std::make_shared<tracker_element_vector>();

    for (auto i : phy_handler_map) {
        auto tracked_phy =
            std::make_shared<tracker_element_map>(phy_phyentry_id);

        auto tracked_name =
            std::make_shared<tracker_element_string>(phy_phyname_id, i.second->fetch_phy_name());
        auto tracked_id =
            std::make_shared<tracker_element_uint32>(phy_phyid_id, i.second->fetch_phy_id());
        auto tracked_dev_count =
            std::make_shared<tracker_element_uint64>(phy_devices_count_id);
        auto tracked_packet_count =
            std::make_shared<tracker_element_uint64>(phy_packets_count_id, phy_packets[i.second->fetch_phy_id()]);

        auto pv_key = phy_view_map.find(i.second->fetch_phy_id());
        if (pv_key != phy_view_map.end())
            tracked_dev_count->set(pv_key->second->get_list_sz());

        tracked_phy->insert(tracked_name);
        tracked_phy->insert(tracked_id);
        tracked_phy->insert(tracked_dev_count);
        tracked_phy->insert(tracked_packet_count);

        ret_vec->push_back(tracked_phy);

    }

    return ret_vec;
}

