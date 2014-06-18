#ifndef __FGLIB_H__
#define __FGLIB_H__

/*
 * Copyright 2014 Open Connectome Project (http://openconnecto.me)
 * Written by Da Zheng (zhengda1936@gmail.com)
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
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "graph_engine.h"
#include "graph.h"
#include "FG_vector.h"

/**
  * \brief A user-friendly wrapper for FlashGraph's raw graph type.
  *         Very usefule when when utilizing FlashGraph 
  *         prewritten/Library algorithms.
  *
*/
class FG_graph
{
	std::string graph_file;
	std::string index_file;
	config_map configs;
	FG_graph(const std::string &graph_file,
			const std::string &index_file, const config_map &configs) {
		this->graph_file = graph_file;
		this->index_file = index_file;
		this->configs = configs;
	}
public:
	typedef std::shared_ptr<FG_graph> ptr; /**Smart pointer through which object is accessed*/

/**
  * \brief  Method to instantiate a graph object.
  *         This method is used in lieu of explicitly calling a ctor.
  *    
  * \param graph_file Path to the graph file on disk.
  * \param index_file Path to the graph index file on disk.
  * \param configs Configuration in configuration file.
*/
	static ptr create(const std::string &graph_file,
			const std::string &index_file, const config_map &configs) {
		return ptr(new FG_graph(graph_file, index_file, configs));
	}

/**
  * \brief Get the path to the graph file.
  *
  * \return The path to the graph file on disk.
  *
*/
	const std::string &get_graph_file() const {
		return graph_file;
	}

/**
  * \brief Get the graph index file path.
  *
  * \return The path to the graph index file on disk.
*/
	const std::string &get_index_file() const {
		return index_file;
	}

/**
  * \brief Get the map that contains the runtime configurations 
  *        for FlashGraph.
  * 
  * \return The config_map that contains all FlashGraph configurations.
  *
*/
	const config_map &get_configs() const {
		return configs;
	}
};

/**
  * \brief Triangle computation type.
  *         
  * - CYCLE triangles are defined for directed graphs and 
  *         depend on the direction of each edge. All edges must be
  *         head to tail connections.
  *     E.g A -----> B
  *         ^     /
  *         |   /
  *         | v
  *         C  
  *                
  * - ALL triangles. Edge direction is disregarded. 
  *     E.g A ----- B
  *         |     /
  *         |   /
  *         | /
  *         C  
  */
enum directed_triangle_type
{
	CYCLE,
	ALL,
};

/**
  * \brief Compute the weakly connectected components of a graph.
  *
  * \param fg The FlashGraph graph object for which you want to compute.
  * \return A vector with an entry for each vertex in the graph's WCC.
  *
*/
FG_vector<vertex_id_t>::ptr compute_wcc(FG_graph::ptr fg);

/**
  * \brief Compute the strongly connected components of a graph.
  *
  * \param fg The FlashGraph graph object for which you want to compute.
  * \return A vector with an entry for each vertex in the graph's SCC.
  *
*/
FG_vector<vertex_id_t>::ptr compute_scc(FG_graph::ptr fg);

/**
  * \brief Compute the directed triangle count for each each vertex.
  *
  * \param fg The FlashGraph graph object for which you want to compute.
  * \param type The type of triangles you wish to count.
  * \return A vector with an entry for each vertex in the graph's triangle
  *         count.
  *
*/
FG_vector<size_t>::ptr compute_directed_triangles(FG_graph::ptr fg,
		directed_triangle_type type);
/**
  * \brief Compute undirected triangle count for each vertex.
  * 
  * \param fg The FlashGraph graph object for which you want to compute.
  * \return A vector with an entry for each vertex in the graph's 
  *         undirected triangle count.
  *
*/
FG_vector<size_t>::ptr compute_undirected_triangles(FG_graph::ptr fg);

/**
  * \brief Compute the per vertex local Scan Statistic 
  * \param fg The FlashGraph graph object for which you want to compute.
  * \return A vector with an entry for each vertex in the graph's 
  *          local scan value.
  *
*/
FG_vector<size_t>::ptr compute_local_scan(FG_graph::ptr);

/**
  * \brief Obtain the top K vertices with the largest local Scan 
  *     Statistic value. 
  * \param fg The FlashGraph graph object for which you want to compute.
  * \param topK The value for K used for the `top K` vertices.  
  * \return A vector of std::pair with an entry for each vertex 
  *         in the top K together with its value.
  *
*/
FG_vector<std::pair<vertex_id_t, size_t> >::ptr compute_topK_scan(
		FG_graph::ptr, size_t topK);

/**
  * \brief Compute the diameter estimation for a graph. 
  * \param fg The FlashGraph graph object for which you want to compute.
  * \return The diameter estimate value.
  *
*/
size_t estimate_diameter(FG_graph::ptr fg, int num_bfs, bool directed,
		int num_sweeps);

/**
  * \brief Compute the PageRank of a graph using the pull method
  *       where vertices request the data from all their neighbors
  *       each iteration. Tends to converge to stable values.
  *
  * \param fg The FlashGraph graph object for which you want to compute.
  * \return A vector with an entry for each vertex in the graph's
  *         PageRank value.
  *
*/
FG_vector<float>::ptr compute_pagerank(FG_graph::ptr fg, int num_iters,
		float damping_factor);

/**
  * \brief Compute the PageRank of a graph using the push method
  *       where vertices send deltas of their PageRank to neighbors
  *       in the event their own PageRank changes.
  *
  * \param fg The FlashGraph graph object for which you want to compute.
  * \param num_iters The maximum number of iterations for PageRank.
  * \param damping_factor The damping factor. Originally .85.
  *
  * \return A vector with an entry for each vertex in the graph's
  *         PageRank value.
  *
*/
FG_vector<float>::ptr compute_pagerank2(FG_graph::ptr, int num_iters,
		float damping_factor);

/**
 * \brief Fetch the clusters with the wanted cluster IDs.
 *  
 * \param fg The FlashGraph graph object for which you want to compute.

 * 
 */
void fetch_subgraphs(FG_graph::ptr graph, FG_vector<vertex_id_t>::ptr comp_ids,
		const std::set<vertex_id_t> &wanted_clusters, std::map<vertex_id_t,
		graph::ptr> &clusters);

/**
 * Compute the size of each subgraph identified by cluster IDs.
 */
void compute_subgraph_sizes(FG_graph::ptr graph, FG_vector<vertex_id_t>::ptr comp_ids,
		const std::set<vertex_id_t> &wanted_clusters,
		std::map<vertex_id_t, std::pair<size_t, size_t> > &sizes);

#endif
