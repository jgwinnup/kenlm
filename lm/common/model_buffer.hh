#ifndef LM_COMMON_MODEL_BUFFER_H
#define LM_COMMON_MODEL_BUFFER_H

/* Format with separate files in suffix order.  Each file contains
 * n-grams of the same order.
 */

#include "util/file.hh"
#include "util/fixed_array.hh"

#include <string>
#include <vector>

namespace util { namespace stream { class Chains; } }

namespace lm {

class ModelBuffer {
  public:
    // Construct for writing.  Must call VocabFile() and fill it with null-delimited vocab words.
    ModelBuffer(StringPiece file_base, bool keep_buffer, bool output_q);

    // Load from file.
    explicit ModelBuffer(StringPiece file_base);

    // Must call VocabFile and populate before calling this function.
    void Sink(util::stream::Chains &chains, const std::vector<uint64_t> &counts);

    void Source(util::stream::Chains &chains);

    // The order of the n-gram model that is associated with the model buffer.
    std::size_t Order() const { return counts_.size(); }
    // Requires Sink or load from file.
    const std::vector<uint64_t> &Counts() const {
      assert(!counts_.empty());
      return counts_;
    }

    int VocabFile() const { return vocab_file_.get(); }

    bool Keep() const { return keep_buffer_; }

  private:
    const std::string file_base_;
    const bool keep_buffer_;
    bool output_q_;
    std::vector<uint64_t> counts_;

    util::scoped_fd vocab_file_;
    util::FixedArray<util::scoped_fd> files_;
};

} // namespace lm

#endif // LM_COMMON_MODEL_BUFFER_H
