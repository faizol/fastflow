/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/* ***************************************************************************
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as 
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 *  As a special exception, you may use this file as part of a free software
 *  library without restriction.  Specifically, if other files instantiate
 *  templates or use macros or inline functions from this file, or you compile
 *  this file and link it with other files to produce an executable, this
 *  file does not by itself cause the resulting executable to be covered by
 *  the GNU General Public License.  This exception does not however
 *  invalidate any other reasons why the executable file might be covered by
 *  the GNU General Public License.
 *
 ****************************************************************************
 */
/*  
 * Test checking the optimizations of pipeline inside a farm.
 *
 *   pipe(First, farm(pipe(Stage, farm(Worker))), Last)
 *
 *
 */
/* Author: Massimo Torquati
 *
 */

#include <string>
#include <iostream>
#include <ff/pipeline.hpp>
#include <ff/farm.hpp>
#include <ff/optimize.hpp>

using namespace ff;

struct First: ff_node_t<long> {
    First(const int ntasks):ntasks(ntasks) {}
    long* svc(long*in) {
        for(long i=1;i<=ntasks;++i) {
            struct timespec req;
            req.tv_sec = 0;
            req.tv_nsec = 3000;
            nanosleep(&req, (struct timespec *)NULL);

            ff_send_out((long*)i);
        }
        return EOS;
    }
    const int ntasks;
};

struct Stage: ff_node_t<long> {
    long* svc(long* in) { return in; }    
};

struct Worker: ff_node_t<long> {
    long* svc(long*in) {
        switch(get_my_id()) {
        case 0: {
            struct timespec req;
            req.tv_sec = 0;
            req.tv_nsec = 50000;
            nanosleep(&req, (struct timespec *)NULL);
        } break;
        case 2: {
            struct timespec req;
            req.tv_sec = 0;
            req.tv_nsec = 20000;
            nanosleep(&req, (struct timespec *)NULL);
        } break;
        default: ; // zero work for the others
        }
        return in;
    }
};
struct Last: ff_node_t<long> {
    long* svc(long* in) {
        printf("Last received %ld\n", (long)in);
        return GO_ON;
    }
};

int main(int argc, char* argv[]) {
    // default arguments
    size_t ntasks    = 10000;
    bool   optimize  = true;
    size_t nworkers  = 4;


    if (argc>1) {
        if (argc!=4) {
            error("use: %s ntasks nworkers optimize\n",argv[0]);
            return -1;
        }
        ntasks    = std::stol(argv[1]);
        nworkers  = std::stol(argv[2]);
        optimize  = (std::stol(argv[3])!=0);
    }

    First first(ntasks);
    Last last;

    /* all the following farms have nworkers1 workers */
    ff_Farm<long,long> farm([&]() {	    
	    std::vector<std::unique_ptr<ff_node> > V;
	    for(size_t i=0;i<nworkers;++i) {
		auto stage = make_unique<Stage>();

		std::vector<std::unique_ptr<ff_node> > W;
		for(size_t j=0;j<3;++j)		    
		    W.push_back(make_unique<Worker>());		
		auto ifarm = make_unique<ff_Farm<long,long>>(std::move(W));
		
		auto ipipe = make_unique<ff_Pipe<long,long>>(std::move(stage), std::move(ifarm));
		V.push_back(std::move(ipipe));
	    }
	    return V;
        } ());
    
    // original network
    ff_Pipe<> pipe(first, farm, last);
    
    // optimization that I would like to apply, if possible
    if (optimize) {
        OptLevel opt;
        opt.max_nb_threads=ff_realNumCores();
        opt.verbose_level=2;
        opt.no_initial_barrier=true;
        opt.blocking_mode     =true;   // enabling blocking if #threads > max_nb_threads
        opt.merge_with_emitter=true;   // merging previous pipeline stage with farm emitter 
        opt.remove_collector  =true;   // remove farm collector
        
        // this call tries to apply all previous optimizations modifying the pipe passed
        // as parameter
        if (optimize_static(pipe,opt)<0) {
            error("optimize_static\n");
            return -1;
         }
    }

    printf("Pipe cardinality: %d\n", pipe.cardinality());
    
    // running the optimized pipe
    if (pipe.run_and_wait_end()<0) {
        error("running pipeline\n");
        return -1;
    }
    printf("test DONE\n");
    return 0;
}
