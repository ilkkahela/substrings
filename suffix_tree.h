#pragma once
#include <ostream>
#include <string>
#include <vector>

struct SuffixTreeNode {
    int parent = -1;
    int depth = 0;          // string depth (length of path label from root)
    int leaf_sa_idx = -1;   // SA index for leaves, -1 for internal nodes
    int suffix_link = -1;   // internal nodes only; root links to itself
    std::vector<int> children{};
};

// Suffix tree of text + '\x01' sentinel, built from its suffix and LCP arrays.
// Node 0 is the root.
struct SuffixTree {
    std::string text;               // includes the sentinel
    std::vector<int> sa;
    std::vector<SuffixTreeNode> nodes;
    std::vector<int> any_leaf_pos;  // text position of some leaf below each node
    std::vector<int> leaf_node;     // SA index -> leaf node id

    void writeDot(std::ostream& out) const;
};

[[nodiscard]] SuffixTree buildSuffixTree(const std::string& text);
