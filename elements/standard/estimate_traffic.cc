// -*- c-basic-offset: 4 -*-
/*
 * estimate_traffic.{cc,hh} -- Estimates OCS traffic
 * Matt Mukerjee
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the conditions
 * listed in the Click LICENSE file. These conditions include: you must
 * preserve this copyright notice, and you cannot mention the copyright
 * holders in advertising related to the Software without their permission.
 * The Software is provided WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED. This
 * notice is a summary of the Click LICENSE file; the license in that file is
 * legally binding.
 */

#include <click/config.h>
#include <click/handlercall.hh>
#include <click/args.hh>
#include "estimate_traffic.hh"
#include <click/confparse.hh>
#include <click/error.hh>
#include <click/standard/scheduleinfo.hh>
#include <sys/select.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <signal.h>
#include <errno.h>
#include <pthread.h>

CLICK_DECLS

EstimateTraffic::EstimateTraffic() : _task(this)
{
    pthread_mutex_init(&_lock, NULL);
    pthread_mutex_init(&_adu_lock, NULL);
}

int
EstimateTraffic::configure(Vector<String> &conf, ErrorHandler *errh)
{
    if (Args(conf, this, errh)
        .read_mp("NUM_HOSTS", _num_hosts)
        .read_mp("SOURCE", source)
        .complete() < 0)
        return -1;
    
    if (_num_hosts == 0)
        return -1;

    _traffic_matrix = (long long *)malloc(sizeof(long long) *
                                          _num_hosts * _num_hosts);
    bzero(_traffic_matrix, sizeof(long long) * _num_hosts * _num_hosts);

    _queue_clear_timeout = 1e9;  // 1s
    clock_gettime(CLOCK_MONOTONIC, &_last_queue_clear);

    _print = 0;

    output_traffic_matrix = new String;

    return 0;
}
 
int
EstimateTraffic::initialize(ErrorHandler *errh)
{
    ScheduleInfo::initialize_task(this, &_task, true, errh);
    
#if defined(__linux__)
    sched_setscheduler(getpid(), SCHED_RR, NULL);
#endif

    struct addrinfo hints, *res, *p;
    int yes = 1;

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if (getaddrinfo(NULL, ADU_PORT, &hints, &res) != 0) {
        perror("getaddrinfo() failed");
        pthread_exit(NULL);
    }

    for(p = res; p != NULL; p = p->ai_next) {
        if ((_serverSocket = socket(p->ai_family, p->ai_socktype,
                                    p->ai_protocol)) == -1) {
            perror("Could not open socket");
            continue;
        }

        if (setsockopt(_serverSocket, SOL_SOCKET, SO_REUSEADDR,
                       &yes, sizeof(int)) == -1) {
            perror("Socket setsockopt() failed");
            close(_serverSocket);
            continue;
        }

        if (bind(_serverSocket, p->ai_addr, p->ai_addrlen) == -1) {
            perror("Socket bind() failed");
            close(_serverSocket);
            continue;
        }

        if (listen(_serverSocket, 5) == -1) {
            perror("Socket listen() failed");
            close(_serverSocket);
            continue;
        }
        break;
    }

    freeaddrinfo(res);

    if (p == NULL) {
        fprintf(stderr, "Could not find a socket to bind to.\n");
        pthread_exit(NULL);
    }

    FD_ZERO(&_active_fd_set);
    FD_SET(_serverSocket, &_active_fd_set);

    _queues = (FullNoteLockQueue **)malloc(sizeof(FullNoteLockQueue *) * _num_hosts * _num_hosts);
    for(int src = 0; src < _num_hosts; src++) {
        for(int dst = 0; dst < _num_hosts; dst++) {
            char handler[500];
            sprintf(handler, "hybrid_switch/q%d%d/q", src+1, dst+1);
	    _queues[src * _num_hosts + dst] = (FullNoteLockQueue *)router()->find(handler);
	    if (!_queues[src * _num_hosts + dst]) {
		printf("failed to find queue \"q%d%d\" in router. exiting...\n", src, dst);
		exit(EXIT_FAILURE);
	    }
        }
    }

    _solstice = (Solstice *)router()->find("sol");

    return 0;
}

bool
EstimateTraffic::run_task(Task *)
{
    while(1) {
        // gather traffic matrix from queues
        int clientSocket;
        int i;
        int nbytes;
        struct traffic_info info;
        struct sockaddr_storage *clientAddr;
        socklen_t sinSize = sizeof(struct sockaddr_storage);
        fd_set read_fd_set;
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;

        read_fd_set = _active_fd_set;
        int rc = select(FD_SETSIZE, &read_fd_set, NULL, NULL, &timeout);
        if (rc < 0) {
            perror("select");
            exit(EXIT_FAILURE);
        }

        if (rc > 0) {
            for (i = 0; i < FD_SETSIZE; ++i) {
                if (FD_ISSET(i, &read_fd_set)) {
                    if (i == _serverSocket) {
                        clientAddr = (struct sockaddr_storage *)malloc(sinSize);
                        if ((clientSocket = accept(_serverSocket,
                                                   (struct sockaddr *)clientAddr,
                                                   &sinSize)) == -1) {
                            free(clientAddr);
                            perror("Could not accept() connection");
                            exit(EXIT_FAILURE);
                        }
                        FD_SET(clientSocket, &_active_fd_set);
                        // fprintf(stderr, "New connection: %d\n", clientSocket);
                    }
                    else {
                        // assumes that we read all info
                        nbytes = read(i, &info, sizeof(info));
                        if (nbytes == 0) {
                            // fprintf(stderr, "Closing socket\n");
                            close(i);
                            FD_CLR(i, &_active_fd_set);
                            break;
                        }
                        if (nbytes < 0) {
                            perror("Socket read() failed");
                            close(i);
                            FD_CLR(i, &_active_fd_set);
                            exit(EXIT_FAILURE);
                        }
			// char src[INET_ADDRSTRLEN], dst[INET_ADDRSTRLEN];
			// inet_ntop(AF_INET, &(info.src), src, INET_ADDRSTRLEN);
			// inet_ntop(AF_INET, &(info.dst), dst, INET_ADDRSTRLEN);
                        // fprintf(stderr, "[CTRL] SRC: %s DST: %s PROTO: %d SPORT: %d DPORT: %d SIZE: %ld\n",
                        //         src, dst, info.proto, info.sport, info.dport, info.size);
                        if (nbytes != sizeof(info)) {
                            fprintf(stderr, "did not read enough bytes from ADU socket... exiting\n");
                            exit(EXIT_FAILURE);
                        }

			pthread_mutex_lock(&_adu_lock);
			if (expected_adu.find(info) == expected_adu.end() && info.size != -1) { // new flow
			    expected_adu[info] = info.size;
			} else {
			    if (info.size == -1) // terminating
				expected_adu[info] = 0;
			    else
				expected_adu[info] += info.size;
			}
			pthread_mutex_unlock(&_adu_lock);
                    }
                }
            }
        }

	bzero(_traffic_matrix, sizeof(long long) * _num_hosts * _num_hosts);
	if (source == "ADU") {
	    pthread_mutex_lock(&_adu_lock);
	    for(auto it = expected_adu.begin(); it != expected_adu.end(); ++it) {
		struct traffic_info info = it->first;
		long long expected_size = it->second;

		uint32_t src_addr = ntohl(info.src.s_addr);
		uint32_t dst_addr = ntohl(info.dst.s_addr);

		uint8_t net_type = (src_addr >> 16) & 0xFF;
		if (net_type != 1) {
		    // fprintf(stderr, "bad net type %u...\n", net_type);
		    continue;
		}

		uint8_t src = ((src_addr >> 8) & 0xFF);
		if (src == 0 || src > _num_hosts) {
		    // fprintf(stderr, "bad src addr %u...\n", src_addr);
		    continue;
		}
		src--;

		uint8_t dst = ((dst_addr >> 8) & 0xFF);
		if (dst == 0 || dst > _num_hosts) {
		    // fprintf(stderr, "bad dst addr %u...\n", dst_addr);
		    continue;
		}
		dst--;

		long long seen_size = _queues[src * _num_hosts + dst]->get_seen_adu(info);

		if (src != dst) {
		    if (expected_size > seen_size)
			_traffic_matrix[src * _num_hosts + dst] += (expected_size - seen_size);
		}
	    }
	    pthread_mutex_unlock(&_adu_lock);
	} else {
	    for (int src = 0; src < _num_hosts; src++) {
                for (int dst = 0; dst < _num_hosts; dst++) {
                    int i = src * _num_hosts + dst;
                    _traffic_matrix[i] += _queues[i]->get_bytes();
                    if (_traffic_matrix[i] < 0)
                        _traffic_matrix[i] = 0;
                }
            }
        }

        // copy TM to store for handler
        String *tm = new String;
        for(int src = 0; src < _num_hosts; src++) {
            for(int dst = 0; dst < _num_hosts; dst++) {
                if (*tm != "")
                    *tm += " ";
                *tm += String(_traffic_matrix[src * _num_hosts + dst]);
            }
        }
	pthread_mutex_lock(&_lock);
        String *temp = output_traffic_matrix;
        output_traffic_matrix = tm;
        delete temp;
	pthread_mutex_unlock(&_lock);

        _print = (_print + 1) % 100000;

	// // clear ADUs periodically
	// if (source == "ADU") {
	//     struct timespec ts_new;
	//     clock_gettime(CLOCK_MONOTONIC, &ts_new);
	//     long long current_nano = 1e9 * ts_new.tv_sec + ts_new.tv_nsec;
	//     long long last_nano = 1e9 * _last_queue_clear.tv_sec + _last_queue_clear.tv_nsec;
	//     if (current_nano > last_nano + _queue_clear_timeout) {
	// 	// clear my ADUs
	// 	pthread_mutex_lock(&_adu_lock);
	// 	expected_adu = std::unordered_map<const struct traffic_info, long long,
	// 					  info_key_hash, info_key_equal>();
	// 	pthread_mutex_unlock(&_adu_lock);
	// 	// clear queue ADUs
	// 	for(int src = 0; src < _num_hosts; src++) {
	// 	    for(int dst = 0; dst < _num_hosts; dst++) {
	// 		_queues[src * _num_hosts + dst]->clear_adus();
	// 	    }
	// 	}
	// 	_last_queue_clear = ts_new;
	//     }
	// }
    }
    return true;
}

String
EstimateTraffic::get_traffic(Element *e, void *)
{
    EstimateTraffic *et = static_cast<EstimateTraffic *>(e);
    pthread_mutex_lock(&(et->_lock));
    String out = String(et->output_traffic_matrix->c_str());
    pthread_mutex_unlock(&(et->_lock));
    return out;
}

int
EstimateTraffic::set_source(const String &str, Element *e, void *, ErrorHandler *)
{
    EstimateTraffic *et = static_cast<EstimateTraffic *>(e);
    et->source = String(str);
    bool use_adus = (et->source == "ADU");
    et->_solstice->use_adus = use_adus;
    int num_hosts = et->_num_hosts;
    for (int i = 0; i < num_hosts; i++) {
	for (int j = 0; j < num_hosts; j++) {
	    et->_queues[i * num_hosts + j]->use_adus = use_adus;
	}
    }
    return 0;
}

int
EstimateTraffic::clear(const String &, Element *e, void *, ErrorHandler *)
{
    EstimateTraffic *et = static_cast<EstimateTraffic *>(e);
    int num_hosts = et->_num_hosts;
    bzero(et->_traffic_matrix, sizeof(long long) * num_hosts * num_hosts);
    pthread_mutex_lock(&(et->_adu_lock));
    et->expected_adu = std::unordered_map<const struct traffic_info, long long,
					  info_key_hash, info_key_equal>();
    pthread_mutex_unlock(&(et->_adu_lock));
    return 0;
}

void
EstimateTraffic::add_handlers()
{
    add_write_handler("setSource", set_source, 0);
    add_read_handler("getTraffic", get_traffic, 0);
    add_write_handler("clear", clear, 0);
}

CLICK_ENDDECLS
EXPORT_ELEMENT(EstimateTraffic)
ELEMENT_REQUIRES(userlevel)
