/*
 * Copyright 2015 Open Connectome Project (http://openconnecto.me)
 * Written by Disa Mhembere (disa@jhu.edu)
 *
 * This file is part of FlashGraph.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY CURRENT_KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <signal.h>
#ifdef PROFILER
#include <gperftools/profiler.h>
#endif

#include "sem_kmeans.h"

// TODO: Opt Assign cluster ID to kms++ selected vertices in ADDMEAN phase
using namespace fg;

#if KM_TEST
static std::string g_fn = "";
static prune_stats::ptr g_prune_stats;
#endif
#if PRUNE
static bool g_prune_init = false;
static dist_matrix::ptr g_cluster_dist;
#endif
#if IOTEST
static unsigned g_io_reqs = 0;
#endif


namespace {
    typedef std::pair<double, double> distpair;
    static unsigned NUM_COLS;
    static unsigned NUM_ROWS;
    static unsigned K;
    static unsigned g_num_changed = 0;
    static struct timeval start, end;
    static std::map<vertex_id_t, unsigned> g_init_hash; // Used for forgy init
    static unsigned  g_kmspp_cluster_idx; // Used for kmeans++ init
    static unsigned g_kmspp_next_cluster; // Sample row selected as the next cluster
    static std::vector<double> g_kmspp_distance; // Used for kmeans++ init

    enum dist_type_t { EUCL, COS }; // Euclidean, Cosine distance
    enum init_type_t { RANDOM, FORGY, PLUSPLUS } g_init; // May have to use
    enum kmspp_stage_t { ADDMEAN, DIST } g_kmspp_stage; // Either adding a mean / computing dist
    enum kms_stage_t { INIT, ESTEP } g_stage; // What phase of the algo we're in

    static std::vector<cluster::ptr> g_clusters; // cluster means/centers
    static const unsigned INVALID_CLUST_ID = -1;
    static unsigned g_iter;

    // Begin Helpers //
#if VERBOSE
    static void print_sample(vertex_id_t my_id, data_seq_iter& count_it) {
        std::vector<std::string> v;
        while (count_it.has_next()) {
            char buffer [1024];
            double e = count_it.next();
            assert(sprintf(buffer, "%e", e));
            v.push_back(std::string(buffer));
        }
        printf("V%u's vector: \n", my_id); print_vector<std::string>(v);
    }
#endif
    // End helpers //

    class kmeans_vertex: public compute_vertex
    {
        unsigned cluster_id;
        double dist;
#if PRUNE
        std::vector<double> lwr_bnd;
        bool recalculated;
#endif
        public:
        kmeans_vertex(vertex_id_t id):
            compute_vertex(id) {
                dist = std::numeric_limits<double>::max(); // Start @ max
                cluster_id = INVALID_CLUST_ID;
#if PRUNE
                lwr_bnd.assign(K, 0); // Set K items to 0
                recalculated = false;
#endif
            }

        unsigned get_result() const {
            return cluster_id;
        }

        const vsize_t get_cluster_id() const {
            return cluster_id;
        }

        void run(vertex_program &prog);

        void run(vertex_program& prog, const page_vertex &vertex) {
            switch (g_stage) {
                case INIT:
                    run_init(prog, vertex, g_init);
                    break;
                case ESTEP:
                    run_distance(prog, vertex);
                    break;
                default:
                    assert(0);
            }
        }

        // Set a cluster to have the same mean as this sample
        void set_as_mean(const page_vertex &vertex, vertex_id_t my_id, unsigned to_cluster_id) {
            vertex_id_t nid = 0;
            data_seq_iter count_it = ((const page_row&)vertex).
                get_data_seq_it<double>();

            // Build the setter vector that we assign to a cluster center
            std::vector<double> setter;
            setter.assign(NUM_COLS, 0);
            while (count_it.has_next()) {
                double e = count_it.next();
                setter[nid++] = e;
            }
            g_clusters[to_cluster_id]->set_mean(setter);
        }

        void run_on_message(vertex_program& prog, const vertex_message& msg) { }
        void run_init(vertex_program& prog, const page_vertex &vertex, init_type_t init);
        void run_distance(vertex_program& prog, const page_vertex &vertex);
        double get_distance(unsigned cl, data_seq_iter& count_it);
#if PRUNE
        double dist_comp(const page_vertex &vertex, const unsigned cl);
#else
        void dist_comp(const page_vertex &vertex, double* best,
                unsigned* new_cluster_id, const unsigned cl);
#endif
    };

    /* Used in per thread cluster formation */
    class kmeans_vertex_program : public vertex_program_impl<kmeans_vertex>
    {

        unsigned pt_changed;
#if IOTEST
        unsigned num_reqs;
#endif

        std::vector<cluster::ptr> pt_clusters;

#if KM_TEST && PRUNE
        prune_stats::ptr pt_ps;
#endif

        public:
        typedef std::shared_ptr<kmeans_vertex_program> ptr;

        //TODO: Opt only add cluster when a vertex joins it
        kmeans_vertex_program(std::vector<cluster::ptr>& pt_clusters) {

            this->pt_clusters = pt_clusters;
            this->pt_changed = 0;
#if IOTEST
            this->num_reqs = 0;
#endif

#if KM_TEST && PRUNE
            pt_ps = prune_stats::create(NUM_ROWS, K);
#endif
        }

        static ptr cast2(vertex_program::ptr prog) {
            return std::static_pointer_cast<kmeans_vertex_program, vertex_program>(prog);
        }

        const std::vector<cluster::ptr>& get_pt_clusters() {
            return pt_clusters;
        }

        void add_member(const unsigned id, data_seq_iter& count_it) {
            pt_clusters[id]->add_member(count_it);
        }

#if PRUNE
        void remove_member(const unsigned id, data_seq_iter& count_it) {
            pt_clusters[id]->remove_member(count_it);
        }

        template <typename T>
            void swap_membership(const unsigned from_id, const unsigned to_id,
                    T& count_it) {
                vertex_id_t nid = 0;
                while(count_it.has_next()) {
                    double e = count_it.next();
                    (*(pt_clusters[from_id]))[nid] -= e;
                    (*(pt_clusters[to_id]))[nid++] += e;
                }

                pt_clusters[from_id]->num_members_peq(-1);
                pt_clusters[to_id]->num_members_peq(1);
            }

#if KM_TEST
        prune_stats::ptr get_ps() { return pt_ps; }
#endif
#endif
        const unsigned get_pt_changed() { return pt_changed; }

        void pt_changed_pp() {
            pt_changed++;
        }

#if IOTEST
        void num_requests_pp() {
            num_reqs++;
        }
        const unsigned get_num_reqs() const { return num_reqs; }
#endif
    };

    class kmeans_vertex_program_creater: public vertex_program_creater
    {
        public:
            vertex_program::ptr create() const {

                std::vector<cluster::ptr> pt_clusters;
                for (unsigned thd = 0; thd < K; thd++) {
                    pt_clusters.push_back(cluster::create(NUM_COLS));
                }

                return vertex_program::ptr(new kmeans_vertex_program(pt_clusters));
            }
    };

    /* Used in kmeans++ initialization */
    class kmeanspp_vertex_program : public vertex_program_impl<kmeans_vertex>
    {
        double pt_cuml_sum;

        public:
        typedef std::shared_ptr<kmeanspp_vertex_program> ptr;

        kmeanspp_vertex_program() {
            pt_cuml_sum = 0.0;
        }

        static ptr cast2(vertex_program::ptr prog) {
            return std::static_pointer_cast<kmeanspp_vertex_program, vertex_program>(prog);
        }

        void pt_cuml_sum_peq (double val) {
            pt_cuml_sum += val;
        }

        const double get_pt_cuml_sum() const {
            return pt_cuml_sum;
        }
    };

    class kmeanspp_vertex_program_creater: public vertex_program_creater
    {
        public:
            vertex_program::ptr create() const {
                return vertex_program::ptr(new kmeanspp_vertex_program());
            }
    };

    void kmeans_vertex::run(vertex_program &prog) {

#if PRUNE
        recalculated = false;
        if (!g_prune_init) {
            for (unsigned cl = 0; cl < K; cl++) {
                // TODO: Test if (g_clusters[cl]->get_prev_dist()) > 0
                lwr_bnd[cl] = std::max((lwr_bnd[cl] - g_clusters[cl]->get_prev_dist()), 0.0);
            }

            /* #6 */
            dist += g_clusters[cluster_id]->get_prev_dist();

            if (dist <= g_clusters[cluster_id]->get_s_val()) {
#if KM_TEST
                kmeans_vertex_program& vprog = (kmeans_vertex_program&) prog;
                vprog.get_ps()->pp_lemma1(K);
#endif
                return; // Nothing changes -- no I/O request!
            }
        }
#endif

#if IOTEST
        kmeans_vertex_program& vprog = (kmeans_vertex_program&) prog;
        vprog.num_requests_pp();
#endif
        vertex_id_t id = prog.get_vertex_id(*this);
        request_vertices(&id, 1);
    }

    void kmeans_vertex::run_init(vertex_program& prog, const page_vertex &vertex, init_type_t init) {
        switch (g_init) {
            case RANDOM:
                {
                    unsigned new_cluster_id = random() % K;
                    kmeans_vertex_program& vprog = (kmeans_vertex_program&) prog;
#if KM_TEST
                    printf("Random init: v%u assigned to cluster: c%x\n",
                            prog.get_vertex_id(*this), new_cluster_id);
#endif
                    this->cluster_id = new_cluster_id;
                    data_seq_iter count_it = ((const page_row&)vertex).
                        get_data_seq_it<double>();
                    vprog.add_member(cluster_id, count_it);
                }
                break;
            case FORGY:
                {
                    vertex_id_t my_id = prog.get_vertex_id(*this);
#if KM_TEST
                    printf("Forgy init: v%u setting cluster: c%x\n", my_id, g_init_hash[my_id]);
#endif
                    set_as_mean(vertex, my_id, g_init_hash[my_id]);
                }
                break;
            case PLUSPLUS:
                {
                    data_seq_iter count_it = ((const page_row&)vertex).
                        get_data_seq_it<double>();

                    if (g_kmspp_stage == ADDMEAN) {
#if KM_TEST
                        vertex_id_t my_id = prog.get_vertex_id(*this);
                        printf("kms++ v%u making itself c%u\n", my_id, g_kmspp_cluster_idx);
#endif
                        g_clusters[g_kmspp_cluster_idx]->add_member(count_it);
                    } else {
                        // FIXME: Opt Test putting if (my_id != g_kmspp_next_cluster) test
                        vertex_id_t my_id = prog.get_vertex_id(*this);
                        double dist = get_distance(g_kmspp_cluster_idx, count_it);
                        if (dist < g_kmspp_distance[my_id]) {
#if VERBOSE
                            printf("kms++ v%u updating dist from: %.3f to %.3f\n",
                                    my_id, g_kmspp_distance[my_id], dist);
#endif
                            g_kmspp_distance[my_id] = dist;
                        }
                    }
                }
                break;
            default:
                assert(0);
        }
    }

    double kmeans_vertex::get_distance(unsigned cl,
            data_seq_iter& count_it) {
        double dist = 0;
        double diff;
        vertex_id_t nid = 0;

        while(count_it.has_next()) {
            double e = count_it.next();
            diff = e - (*g_clusters[cl])[nid++];
            dist += diff*diff;
        }
        return sqrt(dist); // TODO: sqrt
    }

#if PRUNE
    double kmeans_vertex::dist_comp(const page_vertex &vertex, const unsigned cl) {
        data_seq_iter count_it =
            ((const page_row&)vertex).get_data_seq_it<double>();

        return get_distance(cl, count_it);
    }
#else
    void kmeans_vertex::dist_comp(const page_vertex &vertex, double* best,
            unsigned* new_cluster_id, const unsigned cl) {
        data_seq_iter count_it =
            ((const page_row&)vertex).get_data_seq_it<double>();

        double dist = get_distance(cl, count_it);

        if (dist < *best) { // Get the distance to cluster `cl'
            *new_cluster_id = cl;
            *best = dist;
        }
    }
#endif

    void kmeans_vertex::run_distance(vertex_program& prog, const page_vertex &vertex) {
        kmeans_vertex_program& vprog = (kmeans_vertex_program&) prog;
#if PRUNE
        unsigned old_cluster_id = cluster_id;

        if (g_prune_init) {
            for (unsigned cl = 0; cl < K; cl++) {
                double udist = dist_comp(vertex, cl);
                if (udist < dist) {
                    dist = udist;
                    cluster_id = cl;
                }
            }
        } else {
            for (unsigned cl = 0; cl < K; cl++) {
                if (dist <= g_cluster_dist->get(cluster_id, cl)) {
#if KM_TEST
                    vprog.get_ps()->pp_3a();
#endif
                    continue;
                } else if (dist <= lwr_bnd[cl]) {
#if KM_TEST
                    vprog.get_ps()->pp_3b();
#endif
                    continue;
                }

                // If not recalculated to my current cluster .. do so to tighten bounds
                if (!recalculated) {
                    double udist = dist_comp(vertex, cluster_id);
                    lwr_bnd[cluster_id] = udist;
                    dist = udist; // NOTE: No more best!
                    recalculated = true;
                }

                if (dist <= g_cluster_dist->get(cluster_id, cl)) {
#if KM_TEST
                    vprog.get_ps()->pp_3c();
#endif
                    continue;
                }

                // Track 4
                if (lwr_bnd[cl] >= dist) {
#if KM_TEST
                    vprog.get_ps()->pp_4();
#endif
                    continue;
                }

                // Track 5
                double jdist = dist_comp(vertex, cl);
                lwr_bnd[cl] = jdist;
                if (jdist < dist) {
                    dist = jdist;
                    cluster_id = cl;
                }
            }
        }
#else
        double best = std::numeric_limits<double>::max();
        unsigned new_cluster_id = INVALID_CLUST_ID;

        for (unsigned cl = 0; cl < K; cl++) {
            dist_comp(vertex, &best, &new_cluster_id, cl);
        }
#endif

#if PRUNE
        BOOST_VERIFY(cluster_id >= 0 && cluster_id < K);
#else
        BOOST_VERIFY(new_cluster_id >= 0 && new_cluster_id < K);
#endif
        data_seq_iter count_it = ((const page_row&)vertex).get_data_seq_it<double>();

#if PRUNE
        if (g_prune_init) {
            vprog.pt_changed_pp(); // Add a vertex to the count of changed ones
            vprog.add_member(cluster_id, count_it);
        } else if (old_cluster_id != this->cluster_id) {
            vprog.pt_changed_pp(); // Add a vertex to the count of changed ones
            vprog.swap_membership(old_cluster_id, cluster_id, count_it);
        }
#else
        if (this->cluster_id != new_cluster_id) {
            vprog.pt_changed_pp(); // Add a vertex to the count of changed ones
        }

        this->cluster_id = new_cluster_id;
        vprog.add_member(cluster_id, count_it);
        this->dist = best;
#endif
    }


        static FG_vector<unsigned>::ptr get_membership(graph_engine::ptr mat) {
            FG_vector<unsigned>::ptr vec = FG_vector<unsigned>::create(mat);
            mat->query_on_all(vertex_query::ptr(new save_query<unsigned, kmeans_vertex>(vec)));
            return vec;
        }

        static void clear_clusters() {
            for (unsigned cl = 0; cl < g_clusters.size(); cl++) {
#if PRUNE
                g_clusters[cl]->set_prev_mean();

                if (g_prune_init) {
                    g_clusters[cl]->clear();
                } else {
                    g_clusters[cl]->unfinalize();
#if VERBOSE
                    std::cout << "Unfinalized g_clusters[thd] ==> ";
                    print_vector<double>(g_clusters[cl]->get_mean());
#endif
                }
#else
                g_clusters[cl]->clear();
#endif
            }
        }

        static void update_clusters(graph_engine::ptr mat, std::vector<unsigned>& num_members_v) {
            clear_clusters();
            std::vector<vertex_program::ptr> kms_clust_progs;
            mat->get_vertex_programs(kms_clust_progs);

            for (unsigned thd = 0; thd < kms_clust_progs.size(); thd++) {
                kmeans_vertex_program::ptr kms_prog = kmeans_vertex_program::cast2(kms_clust_progs[thd]);
                std::vector<cluster::ptr> pt_clusters = kms_prog->get_pt_clusters();
                g_num_changed += kms_prog->get_pt_changed();
#if IOTEST
                g_io_reqs += kms_prog->get_num_reqs();
#endif
#if PRUNE && KM_TEST
                (*g_prune_stats) += (*kms_prog->get_ps());
#endif
                BOOST_VERIFY(g_num_changed <= NUM_ROWS);
                /* Merge the per-thread clusters */
                for (unsigned cl = 0; cl < K; cl++) {
#if KM_TEST && 0
                    std::cout << "pt_clusters[" << cl << "] #"
                        << pt_clusters[cl]->get_num_members() << " ==> ";
                    print_vector<double>(pt_clusters[cl]->get_mean());
#endif
                    *(g_clusters[cl]) += *(pt_clusters[cl]);
                    if (thd == kms_clust_progs.size()-1) {
                        g_clusters[cl]->finalize();
                        num_members_v[cl] = g_clusters[cl]->get_num_members();
#if PRUNE
                        double dist = eucl_dist<std::vector<double>>(&(g_clusters[cl]->get_mean()),
                                &(g_clusters[cl]->get_prev_mean()));
#if KM_TEST
                        BOOST_LOG_TRIVIAL(info) << "Distance to prev mean for c:"
                            << cl << " is " << dist;
                        BOOST_VERIFY(g_clusters[cl]->get_num_members() <= (int)NUM_ROWS);
#endif
                        g_clusters[cl]->set_prev_dist(dist);
#endif
                    }
                }
            }
#if KM_TEST
            int t_members = 0;
            unsigned cl = 0;
            BOOST_FOREACH(cluster::ptr c , g_clusters) {
                t_members += c->get_num_members();
                if (t_members > (int) NUM_ROWS) {
                    BOOST_LOG_TRIVIAL(error) << "[FATAL]: Too many memnbers cluster: "
                        << cl << "/" << K << " at members = " << t_members;
                    BOOST_VERIFY(false);
                }
                cl++;
            }
#endif
        }

        /* During kmeans++ we select a new cluster each iteration
           This step get the next sample selected as a cluster center
           */
        static unsigned kmeanspp_get_next_cluster_id(graph_engine::ptr mat) {
#if KM_TEST
            BOOST_LOG_TRIVIAL(info) << "Assigning new cluster ...";
#endif
            std::vector<vertex_program::ptr> kmspp_progs;
            mat->get_vertex_programs(kmspp_progs);

            double cuml_sum = 0;
            BOOST_FOREACH(vertex_program::ptr vprog, kmspp_progs) {
                kmeanspp_vertex_program::ptr kmspp_prog = kmeanspp_vertex_program::cast2(vprog);
                cuml_sum += kmspp_prog->get_pt_cuml_sum();
            }
            cuml_sum = (cuml_sum * ((double)random())) / (RAND_MAX-1.0);

            g_kmspp_cluster_idx++;

            for (unsigned row = 0; row < NUM_ROWS; row++) {
#if VERBOSE
                BOOST_LOG_TRIVIAL(info) << "cuml_sum = " << cuml_sum;
#endif
                cuml_sum -= g_kmspp_distance[row];
                if (cuml_sum <= 0) {
#if KM_TEST
                    BOOST_LOG_TRIVIAL(info) << "Choosing v:" << row << " as center K = " << g_kmspp_cluster_idx;
#endif
                    return row;
                }
            }
            BOOST_VERIFY(false);
        }

        // Return all the cluster means only
        static void get_means(std::vector<std::vector<double>>& means) {
            for (std::vector<cluster::ptr>::iterator it = g_clusters.begin();
                    it != g_clusters.end(); ++it) {
                means.push_back((*it)->get_mean());
            }
        }

        static inline bool fexists(const std::string& name) {
            struct stat buffer;
            return (stat (name.c_str(), &buffer) == 0);
        }
    }

    namespace fg
    {
        sem_kmeans_ret::ptr compute_sem_kmeans(FG_graph::ptr fg, const size_t k, const std::string init,
                const unsigned max_iters, const double tolerance, const unsigned num_rows,
                const unsigned num_cols) {
#ifdef PROFILER
            ProfilerStart("/home/disa/FlashGraph/flash-graph/libgraph-algs/sem_kmeans.perf");
#endif
            K = k;

            // Check Initialization
            if (init.compare("random") && init.compare("kmeanspp") &&
                    init.compare("forgy")) {
                BOOST_LOG_TRIVIAL(fatal)
                    << "[ERROR]: param init must be one of: 'random', 'forgy', 'kmeanspp'.It is '"
                    << init << "'";
                exit(EXIT_FAILURE);
            }

            graph_index::ptr index = NUMA_graph_index<kmeans_vertex>::create(
                    fg->get_graph_header());
            graph_engine::ptr mat = fg->create_engine(index);

            NUM_ROWS = mat->get_max_vertex_id() + 1;
            NUM_COLS = num_cols;

            // Check k
            if (K > NUM_ROWS || K < 2 || K == (unsigned)-1) {
                BOOST_LOG_TRIVIAL(fatal)
                    << "'k' must be between 2 and the number of rows in the matrix " <<
                    "k = " << K;
                exit(EXIT_FAILURE);
            }

            BOOST_VERIFY(num_cols > 0);

#if KM_TEST
            g_prune_stats = prune_stats::create(NUM_ROWS, K);
            BOOST_LOG_TRIVIAL(info) << "We have rows = " << NUM_ROWS << ", cols = " <<
                NUM_COLS;
            g_fn = "/mnt/nfs/disa/FlashGraph/flash-graph/test-algs/clusters_r"+std::to_string(NUM_ROWS)\
                  +"_c"+std::to_string(NUM_COLS)+".bin";
#endif

            gettimeofday(&start , NULL);
            /*** Begin VarInit of data structures ***/

#if MAT_TEST
            std::string init_centers_fn = "/mnt/nfs/disa/data/tiny/fkms_data/5c_95413.bin";
            bin_reader<double> br(init_centers_fn, 5, 57);
#endif

            FG_vector<unsigned>::ptr cluster_assignments; // Which cluster a sample is in
            for (size_t cl = 0; cl < k; cl++) {
#if MAT_TEST
                std::vector<double> v = br.readline();
                g_clusters.push_back(cluster::create(v));
#else
                g_clusters.push_back(cluster::create(NUM_COLS));
#endif
            }

            std::vector<unsigned> num_members_v;
            num_members_v.resize(K);

#if PRUNE
            BOOST_LOG_TRIVIAL(info) << "Init of g_cluster_dist";
            // Distance to everyone other than yourself
            g_cluster_dist = dist_matrix::create(K);
#endif
            /*** End VarInit ***/
            g_stage = INIT;

#if !MAT_TEST
            if (init == "random") {
                BOOST_LOG_TRIVIAL(info) << "Running init: '"<< init <<"' ...";
                g_init = RANDOM;

                mat->start_all(vertex_initializer::ptr(),
                        vertex_program_creater::ptr(new kmeans_vertex_program_creater()));
                mat->wait4complete();

                update_clusters(mat, num_members_v);
            }
            if (init == "forgy") {
                BOOST_LOG_TRIVIAL(info) << "Deterministic Init is: '"<< init <<"'";
                g_init = FORGY;

                // Select K in range NUM_ROWS
                std::vector<vertex_id_t> init_ids; // Used to start engine
                for (unsigned cl = 0; cl < K; cl++) {
                    vertex_id_t id = random() % NUM_ROWS;
                    g_init_hash[id] = cl; // <vertex_id, cluster_id>
                    init_ids.push_back(id);
                }
                mat->start(&init_ids.front(), K);
                mat->wait4complete();

            } else if (init == "kmeanspp") {
                BOOST_LOG_TRIVIAL(info) << "Init is '"<< init <<"'";
                g_init = PLUSPLUS;

                // Init g_kmspp_distance to max distance
                g_kmspp_distance.assign(NUM_ROWS, std::numeric_limits<double>::max());

                g_kmspp_cluster_idx = 0;
                g_kmspp_next_cluster = random() % NUM_ROWS;
#if KM_TEST
                BOOST_LOG_TRIVIAL(info) << "Assigning v:" << g_kmspp_next_cluster << " as first cluster";
#endif
                g_kmspp_distance[g_kmspp_next_cluster] = 0;

                // Fire up K engines with 2 iters/engine
                while (true) {
#if KM_TEST && 0
                    BOOST_LOG_TRIVIAL(info) << "Printing updated distances";
                    print_vector<double>(g_kmspp_distance);
#endif
                    // TODO: Start 1 vertex which will activate all
                    g_kmspp_stage = ADDMEAN;

                    mat->start(&g_kmspp_next_cluster, 1, vertex_initializer::ptr(),
                            vertex_program_creater::ptr(new kmeanspp_vertex_program_creater()));
                    mat->wait4complete();
                    g_clusters[g_kmspp_cluster_idx]->num_members_peq(-1);

#if KM_TEST
                    BOOST_LOG_TRIVIAL(info) << "Printing clusters after sample set_mean ...";
                    print_clusters(g_clusters);
                    BOOST_VERIFY(g_clusters[g_kmspp_cluster_idx]->get_num_members() == 0);
#endif
                    if (g_kmspp_cluster_idx+1 == K) { break; } // skip the distance comp since we picked clusters
                    g_kmspp_stage = DIST;
                    mat->start_all(); // Only need a vanilla vertex_program
                    mat->wait4complete();

                    g_kmspp_next_cluster = kmeanspp_get_next_cluster_id(mat);
                }
            }
#endif

#if PRUNE
            if (init == "forgy" || init == "kmeanspp") {
                g_prune_init = true; // set
                g_stage = ESTEP;
                BOOST_LOG_TRIVIAL(info) << "Init: Computing cluster distance matrix ...";
                g_cluster_dist->compute_dist(g_clusters, K);
#if KM_TEST
                BOOST_LOG_TRIVIAL(info) << "Printing inited cluster distance matrix ...";
                g_cluster_dist->print();
#endif

                BOOST_LOG_TRIVIAL(info) << "Init: Running an engine for PRUNE since init is " << init;

                mat->start_all(vertex_initializer::ptr(),
                        vertex_program_creater::ptr(new kmeans_vertex_program_creater()));
                mat->wait4complete();
                BOOST_LOG_TRIVIAL(info) << "Init: M-step Updating cluster means ...";

                update_clusters(mat, num_members_v);
#if KM_TEST
                BOOST_LOG_TRIVIAL(info) << "Init: printing cluster counts:";
                print_vector<unsigned>(num_members_v);
#endif
                g_prune_init = false; // reset
                g_num_changed = 0; // reset
            }
#endif

#if KM_TEST && 0
            BOOST_LOG_TRIVIAL(info) << "Printing cluster assignments:";
            get_membership(mat)->print(NUM_ROWS);
#endif

            g_stage = ESTEP;
            BOOST_LOG_TRIVIAL(info) << "SEM-K||means starting ...";

            bool converged = false;

            std::string str_iters = max_iters == std::numeric_limits<unsigned>::max() ?
                "until convergence ...":
                std::to_string(max_iters) + " iterations ...";
            BOOST_LOG_TRIVIAL(info) << "Computing " << str_iters;
            g_iter = 1;

            while (g_iter < max_iters) {
#if 0
                if (g_iter == 4) {
                    if (fexists(g_fn)) {
                        BOOST_LOG_TRIVIAL(warning) << "[WARNING]: overwriting " << g_fn;
                    }
                    get_membership(mat)->to_file(g_fn);
                    exit(EXIT_FAILURE); // FIXME: Premature
                }
                if (g_iter == 3) {
                    FG_vector<unsigned>::ptr pruned_memb = get_membership(mat);
                    BOOST_VERIFY(fexists(g_fn));
                    std::unique_ptr<std::vector<size_t> > neq =
                        pruned_memb->where_nequal(FG_vector<unsigned>::from_file(g_fn));

                    printf("Size of not equal: %lu\n", neq->size());
                    print_vector<size_t>(*neq);

                    exit(EXIT_FAILURE); // FIXME: Premature
                }
#endif
                BOOST_LOG_TRIVIAL(info) << "E-step Iteration " << g_iter <<
                    " . Computing cluster assignments ...";
#if PRUNE
                BOOST_LOG_TRIVIAL(info) << "Main: Computing cluster distance matrix ...";
                g_cluster_dist->compute_dist(g_clusters, K);

                for (unsigned cl = 0; cl < K; cl++)
                    BOOST_LOG_TRIVIAL(info) << "cl:" << cl << " get_s_val: " << g_clusters[cl]->get_s_val();
#if VERBOSE
                BOOST_LOG_TRIVIAL(info) << "Cluster distance matrix ...";
                g_cluster_dist->print();
#endif
#endif
                mat->start_all(vertex_initializer::ptr(),
                        vertex_program_creater::ptr(new kmeans_vertex_program_creater()));
                mat->wait4complete();
                BOOST_LOG_TRIVIAL(info) << "Main: M-step Updating cluster means ...";
                update_clusters(mat, num_members_v);

#if KM_TEST && 0
                BOOST_LOG_TRIVIAL(info) << "Printing cluster means:";
                print_clusters(g_clusters);

                BOOST_LOG_TRIVIAL(info) << "Getting cluster membership ...";
                get_membership(mat)->print(NUM_ROWS);
#endif
                BOOST_LOG_TRIVIAL(info) << "Printing cluster counts ...";
                print_vector<unsigned>(num_members_v);

                BOOST_LOG_TRIVIAL(info) << "** Samples changes cluster: " << g_num_changed << " **\n";

                if (g_num_changed == 0 || ((g_num_changed/(double)NUM_ROWS)) <= tolerance) {
                    converged = true;
                    break;
                } else {
                    g_num_changed = 0;
                }
                g_iter++;

#if PRUNE && KM_TEST
                g_prune_stats->finalize();
#endif
            }

#if PRUNE && KM_TEST
            g_prune_stats->get_stats();
#endif
            gettimeofday(&end, NULL);
            BOOST_LOG_TRIVIAL(info) << "\n\nAlgorithmic time taken = " <<
                time_diff(start, end) << " sec\n";

#ifdef PROFILER
            ProfilerStop();
#endif
            BOOST_LOG_TRIVIAL(info) << "\n******************************************\n";
#if IOTEST
            printf("Total # of IO requests: %u\nTotal bytes requested: %lu\n\n",
                    g_io_reqs, (g_io_reqs*(sizeof(double))*NUM_COLS));
#endif

            if (converged) {
                BOOST_LOG_TRIVIAL(info) <<
                    "K-means converged in " << g_iter << " iterations";
            } else {
                BOOST_LOG_TRIVIAL(warning) << "[Warning]: K-means failed to converge in "
                    << g_iter << " iterations";
            }
            BOOST_LOG_TRIVIAL(info) << "\n******************************************\n";

            print_vector<unsigned>(num_members_v);

            std::vector<std::vector<double>> means;
            get_means(means);
            cluster_assignments = get_membership(mat);
#if KM_TEST && 0
            BOOST_LOG_TRIVIAL(info) << "Printing updated distances";
            print_vector<double>(g_kmspp_distance);
#endif
            return sem_kmeans_ret::create(cluster_assignments, means, num_members_v, g_iter);
        }
    }
