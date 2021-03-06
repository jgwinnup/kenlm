#include "lm/interpolate/pipeline.hh"

#include "lm/common/compare.hh"
#include "lm/common/print.hh"
#include "lm/common/renumber.hh"
#include "lm/interpolate/backoff_reunification.hh"
#include "lm/interpolate/interpolate_info.hh"
#include "lm/interpolate/merge_probabilities.hh"
#include "lm/interpolate/merge_vocab.hh"
#include "lm/interpolate/normalize.hh"
#include "lm/interpolate/universal_vocab.hh"
#include "util/stream/chain.hh"
#include "util/stream/count_records.hh"
#include "util/stream/io.hh"
#include "util/stream/multi_stream.hh"
#include "util/stream/sort.hh"
#include "util/fixed_array.hh"

namespace lm { namespace interpolate { namespace {

/* Put the original input files on chains and renumber them */
void SetupInputs(std::size_t buffer_size, const UniversalVocab &vocab, util::FixedArray<ModelBuffer> &models, bool exclude_highest, util::FixedArray<util::stream::Chains> &chains, util::FixedArray<util::stream::ChainPositions> &positions) {
  chains.clear();
  positions.clear();
  // TODO: much better memory sizing heuristics e.g. not making the chain larger than it will use.
  util::stream::ChainConfig config(0, 2, buffer_size);
  for (std::size_t i = 0; i < models.size(); ++i) {
    chains.push_back(models[i].Order() - exclude_highest);
    for (std::size_t j = 0; j < models[i].Order() - exclude_highest; ++j) {
      config.entry_size = sizeof(WordIndex) * (j + 1) + sizeof(float) * 2; // TODO do not include wasteful backoff for highest.
      chains.back().push_back(config);
    }
    models[i].Source(chains.back());
    for (std::size_t j = 0; j < models[i].Order() - exclude_highest; ++j) {
      chains[i][j] >> Renumber(vocab.Mapping(i), j + 1);
    }
  }
 for (std::size_t i = 0; i < chains.size(); ++i) {
    positions.push_back(chains[i]);
  }
}

template <class SortOrder> void ApplySort(const util::stream::SortConfig &config, util::stream::Chains &chains) {
  util::stream::Sorts<SortOrder> sorts(chains.size());
  for (std::size_t i = 0; i < chains.size(); ++i) {
    sorts.push_back(chains[i], config, SortOrder(i + 1));
  }
  chains.Wait(true);
  // TODO memory management
  for (std::size_t i = 0; i < sorts.size(); ++i) {
    sorts[i].Merge(sorts[i].DefaultLazy());
  }
  for (std::size_t i = 0; i < sorts.size(); ++i) {
    sorts[i].Output(chains[i], sorts[i].DefaultLazy());
  }
};

} // namespace

void Pipeline(util::FixedArray<ModelBuffer> &models, const Config &config, int write_file) {
  // Setup InterpolateInfo and UniversalVocab.
  InterpolateInfo info;
  info.lambdas = config.lambdas;
  std::vector<WordIndex> vocab_sizes;
  UniversalVocab vocab(vocab_sizes);
  util::scoped_fd vocab_null(util::MakeTemp(config.sort.temp_prefix));
  std::size_t max_order = 0;
  {
    util::FixedArray<util::scoped_fd> vocab_files;
    for (ModelBuffer *i = models.begin(); i != models.end(); ++i) {
      info.orders.push_back(i->Order());
      vocab_sizes.push_back(i->Counts()[0]);
      vocab_files.push_back(util::DupOrThrow(i->VocabFile()));
      max_order = std::max(max_order, i->Order());
    }
    MergeVocab(vocab_files, vocab, vocab_null.get());
  }

  // Pass 1: merge probabilities
  util::FixedArray<util::stream::Chains> input_chains(models.size());
  util::FixedArray<util::stream::ChainPositions> models_by_order(models.size());
  SetupInputs(config.BufferSize(), vocab, models, false, input_chains, models_by_order);

  util::stream::Chains merged_probs(max_order);
  for (std::size_t i = 0; i < max_order; ++i) {
    merged_probs.push_back(util::stream::ChainConfig(PartialProbGamma::TotalSize(info, i + 1), 2, config.BufferSize())); // TODO: not buffer_size
  }
  MergeProbabilities(info, models_by_order, merged_probs);
  std::vector<uint64_t> counts(max_order);
  for (std::size_t i = 0; i < max_order; ++i) {
    merged_probs[i] >> util::stream::CountRecords(&counts[i]);
  }

  // Pass 2: normalize.
  ApplySort<ContextOrder>(config.sort, merged_probs);
  SetupInputs(config.BufferSize(), vocab, models, true, input_chains, models_by_order);
  util::stream::Chains probabilities(max_order), backoffs(max_order - 1);
  for (std::size_t i = 0; i < max_order; ++i) {
    probabilities.push_back(util::stream::ChainConfig(NGram<float>::TotalSize(i + 1), 2, config.BufferSize()));
  }
  for (std::size_t i = 0; i < max_order - 1; ++i) {
    backoffs.push_back(util::stream::ChainConfig(sizeof(float), 2, config.BufferSize()));
  }
  Normalize(info, models_by_order, merged_probs, probabilities, backoffs);

  util::FixedArray<util::stream::FileBuffer> backoff_buffers(backoffs.size());
  for (std::size_t i = 0; i < max_order - 1; ++i) {
    backoff_buffers.push_back(util::MakeTemp(config.sort.temp_prefix));
    backoffs[i] >> backoff_buffers.back().Sink() >> util::stream::kRecycle;
  }

  // Pass 3: backoffs in the right place.
  ApplySort<SuffixOrder>(config.sort, probabilities);
  // TODO destroy universal vocab to save RAM.
  // TODO these should be freed before merge sort happens in the above function.
  backoffs.Wait(true);
  merged_probs.Wait(true);

  util::stream::ChainPositions prob_pos(max_order - 1);
  util::stream::Chains combined(max_order - 1);
  for (std::size_t i = 0; i < max_order - 1; ++i) {
    backoffs[i] >> backoff_buffers[i].Source(true);
    prob_pos.push_back(probabilities[i].Add());
    combined.push_back(util::stream::ChainConfig(NGram<ProbBackoff>::TotalSize(i + 1), 2, config.BufferSize()));
  }
  util::stream::ChainPositions backoff_pos(backoffs);

  ReunifyBackoff(prob_pos, backoff_pos, combined);

  util::stream::ChainPositions output_pos(max_order);
  for (std::size_t i = 0; i < max_order - 1; ++i) {
    output_pos.push_back(combined[i].Add());
  }
  output_pos.push_back(probabilities.back().Add());

  probabilities >> util::stream::kRecycle;
  backoffs >> util::stream::kRecycle;
  combined >> util::stream::kRecycle;

  // TODO genericize to ModelBuffer etc.
  PrintARPA(vocab_null.get(), write_file, counts).Run(output_pos);
}

}} // namespaces
