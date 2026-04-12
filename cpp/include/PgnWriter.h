#pragma once

#include <string>
#include <vector>

namespace aa {

struct PgnPly {
    std::string san;
    std::string clock;
    std::vector<std::vector<PgnPly>> variations; // Nested analysis lines
};

class PgnWriter {
public:
    PgnWriter();

    // PGN Meta data
    void add_header(const std::string& key, const std::string& value);
    
    // Appends a move to the currently active tree branch
    void add_ply(const std::string& san, const std::string& clock = "");

    // Branches off a new variation from the preceding move
    void push_variation();
    // Returns to the primary active variation line
    void pop_variation();

    std::string build() const;

private:
    void build_line(std::ostringstream& oss, const std::vector<PgnPly>& line, int starting_ply_count, int indent_level) const;

    std::vector<std::pair<std::string, std::string>> headers_;
    std::vector<PgnPly> main_line_;
    std::vector<std::vector<PgnPly>*> active_lines_;
};

} // namespace aa