#include "suffix_tree.h"
#include "suffix_array.h"
#include "utf8.h"
#include <algorithm>
#include <print>
#include <ranges>

using namespace std;

namespace {

constexpr char kSentinel = '\x01';

// A leaf's edge label runs to the end of the text, so unshortened labels
// would make the DOT file quadratic in the input size.
constexpr size_t kMaxLabelBytes = 16;

// Walks the suffixes in sorted order, keeping the path from the root to the
// previous leaf on a stack. The LCP with the previous suffix says how far back
// up that path the next leaf branches off; if no node sits at exactly that
// depth, the edge there is split by a new internal node.
void buildTopology(SuffixTree& t, const vector<int>& lcp) {
    const auto& sa = t.sa;
    int n = static_cast<int>(sa.size());
    auto& tree = t.nodes;
    tree.push_back({});  // root

    auto reparent = [&](int child, int new_parent) {
        auto& pc = tree[tree[child].parent].children;
        pc.erase(remove(pc.begin(), pc.end(), child), pc.end());
        tree[child].parent = new_parent;
        tree[new_parent].children.push_back(child);
    };

    vector<int> path{0};
    for (int i = 0; i < n; i++) {
        int branch_depth = (i == 0) ? 0 : lcp[i];
        int last_popped = -1;

        while (tree[path.back()].depth > branch_depth) {
            last_popped = path.back();
            path.pop_back();
        }

        if (tree[path.back()].depth < branch_depth) {
            int new_id = static_cast<int>(tree.size());
            tree.push_back({.parent = path.back(), .depth = branch_depth});
            tree[path.back()].children.push_back(new_id);
            if (last_popped != -1)
                reparent(last_popped, new_id);
            path.push_back(new_id);
        }

        int leaf_id = static_cast<int>(tree.size());
        tree.push_back({.parent = path.back(), .depth = n - sa[i], .leaf_sa_idx = i});
        tree[path.back()].children.push_back(leaf_id);
        path.push_back(leaf_id);
    }
}

void computeLeafPositions(SuffixTree& t) {
    const auto& tree = t.nodes;
    int total = static_cast<int>(tree.size());

    // Pre-order, then fill bottom-up so each node inherits from its first child.
    vector<int> order;
    order.reserve(total);
    vector<int> pending{0};
    while (!pending.empty()) {
        int v = pending.back();
        pending.pop_back();
        order.push_back(v);
        for (int c : tree[v].children)
            pending.push_back(c);
    }

    t.any_leaf_pos.assign(total, -1);
    for (int v : order | views::reverse) {
        if (tree[v].leaf_sa_idx >= 0)
            t.any_leaf_pos[v] = t.sa[tree[v].leaf_sa_idx];
        else if (!tree[v].children.empty())
            t.any_leaf_pos[v] = t.any_leaf_pos[tree[v].children[0]];
    }

    t.leaf_node.assign(t.sa.size(), -1);
    for (auto [i, node] : tree | views::enumerate) {
        if (node.leaf_sa_idx >= 0)
            t.leaf_node[node.leaf_sa_idx] = static_cast<int>(i);
    }
}

// The suffix link of internal node "xα" is node "α": take any leaf below it
// (suffix at p), hop to the leaf for suffix p+1, and climb to depth |α|.
void computeSuffixLinks(SuffixTree& t, const vector<int>& rank) {
    auto& tree = t.nodes;
    int n = static_cast<int>(t.sa.size());
    tree[0].suffix_link = 0;

    for (int i = 1; i < static_cast<int>(tree.size()); i++) {
        if (tree[i].leaf_sa_idx >= 0) continue;
        int p = t.any_leaf_pos[i];
        if (p < 0 || p + 1 >= n) {
            tree[i].suffix_link = 0;
            continue;
        }
        int cur = t.leaf_node[rank[p + 1]];
        while (cur > 0 && tree[cur].depth > tree[i].depth - 1)
            cur = tree[cur].parent;
        tree[i].suffix_link = cur;
    }
}

// Cuts s to at most kMaxLabelBytes plus "...", without splitting a character.
string shortenLabel(string_view s) {
    if (s.size() <= kMaxLabelBytes) return string(s);
    size_t cut = kMaxLabelBytes;
    while (cut > 0 && isUtf8Continuation(s[cut])) cut--;
    return string(s.substr(0, cut)) + "...";
}

string dotEscape(string_view s) {
    string result;
    for (char c : s) {
        switch (c) {
            case kSentinel: result += '$'; break;
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:   result += c;
        }
    }
    return result;
}

} // namespace

SuffixTree buildSuffixTree(const string& text) {
    SuffixTree t;
    t.text = text + kSentinel;
    SuffixArray sa(t.text);
    t.sa = sa.getSA();

    buildTopology(t, sa.getLCP());
    computeLeafPositions(t);
    computeSuffixLinks(t, sa.getRank());
    return t;
}

void SuffixTree::writeDot(ostream& out) const {
    auto edgeLabel = [&](int parent_depth, int child) {
        int start = any_leaf_pos[child] + parent_depth;
        int len = nodes[child].depth - parent_depth;
        return dotEscape(shortenLabel(string_view(text).substr(start, len)));
    };

    std::println(out, "digraph suffix_tree {{");
    std::println(out, "    rankdir=TB;");
    std::println(out, "    node [fontname=\"monospace\"];\n");

    for (int i = 0; i < static_cast<int>(nodes.size()); i++) {
        if (nodes[i].leaf_sa_idx >= 0)
            std::println(out, "    n{} [shape=box, label=\"{}\"];", i, sa[nodes[i].leaf_sa_idx]);
        else if (i == 0)
            std::println(out, "    n0 [shape=circle, label=\"root\"];");
        else
            std::println(out, "    n{} [shape=point, width=0.15];", i);
    }

    std::println(out, "\n    // Tree edges");
    for (int i = 0; i < static_cast<int>(nodes.size()); i++) {
        for (int child : nodes[i].children)
            std::println(out, "    n{} -> n{} [label=\"{}\"];", i, child, edgeLabel(nodes[i].depth, child));
    }

    std::println(out, "\n    // Suffix links");
    std::println(out, "    edge [style=dashed, color=red, constraint=false];");
    for (int i = 1; i < static_cast<int>(nodes.size()); i++) {
        if (nodes[i].leaf_sa_idx >= 0) continue;
        if (nodes[i].suffix_link >= 0 && nodes[i].suffix_link != i)
            std::println(out, "    n{} -> n{};", i, nodes[i].suffix_link);
    }

    std::println(out, "}}");
}
