#define BOOST_TEST_MODULE InterpolateMergeVocabTest
#include <boost/test/unit_test.hpp>

#include "lm/interpolate/merge_vocab.hh"
#include "lm/interpolate/universal_vocab.hh"
#include "lm/lm_exception.hh"
#include "lm/word_index.hh"
#include "util/file.hh"
#include "util/file_piece.hh"

#include <cstring>

namespace lm {
namespace interpolate {
namespace {

// Stupid bjam permutes the command line arguments randomly.
class TestFiles {
  public:
    TestFiles() {
      char **argv = boost::unit_test::framework::master_test_suite().argv;
      int argc = boost::unit_test::framework::master_test_suite().argc;
      BOOST_REQUIRE_EQUAL(6, argc);
      for (int i = 1; i < argc; ++i) {
        EndsWithAssign(argv[i], "test1", test[0]);
        EndsWithAssign(argv[i], "test2", test[1]);
        EndsWithAssign(argv[i], "test3", test[2]);
        EndsWithAssign(argv[i], "no_unk", no_unk);
        EndsWithAssign(argv[i], "bad_order", bad_order);
      }
    }

    void EndsWithAssign(char *arg, StringPiece value, util::scoped_fd &to) {
      StringPiece str(arg);
      if (str.size() < value.size()) return;
      if (std::memcmp(str.data() + str.size() - value.size(), value.data(), value.size())) return;
      to.reset(util::OpenReadOrThrow(arg));
    }

    util::scoped_fd test[3], no_unk, bad_order;
};

BOOST_AUTO_TEST_CASE(MergeVocabTest) {
  TestFiles files;

  util::FixedArray<util::scoped_fd> used_files(3);
  used_files.push_back(files.test[0].release());
  used_files.push_back(files.test[1].release());
  used_files.push_back(files.test[2].release());

  std::vector<lm::WordIndex> model_max_idx;
  model_max_idx.push_back(10);
  model_max_idx.push_back(10);
  model_max_idx.push_back(10);

  util::scoped_fd combined(util::MakeTemp("temporary"));

  UniversalVocab universal_vocab(model_max_idx);
  MergeVocab(used_files, universal_vocab, combined.get());

  BOOST_CHECK_EQUAL(universal_vocab.GetUniversalIdx(0, 0), 0);
  BOOST_CHECK_EQUAL(universal_vocab.GetUniversalIdx(1, 0), 0);
  BOOST_CHECK_EQUAL(universal_vocab.GetUniversalIdx(2, 0), 0);
  BOOST_CHECK_EQUAL(universal_vocab.GetUniversalIdx(0, 1), 1);
  BOOST_CHECK_EQUAL(universal_vocab.GetUniversalIdx(1, 1), 2);
  BOOST_CHECK_EQUAL(universal_vocab.GetUniversalIdx(2, 1), 8);
  BOOST_CHECK_EQUAL(universal_vocab.GetUniversalIdx(0, 5), 11);
  BOOST_CHECK_EQUAL(universal_vocab.GetUniversalIdx(1, 3), 4);
  BOOST_CHECK_EQUAL(universal_vocab.GetUniversalIdx(2, 3), 10);

  util::FilePiece f(combined.release());
  BOOST_CHECK_EQUAL("<unk>", f.ReadLine('\0'));
  BOOST_CHECK_EQUAL("a", f.ReadLine('\0'));
  BOOST_CHECK_EQUAL("is this", f.ReadLine('\0'));
  BOOST_CHECK_EQUAL("this a", f.ReadLine('\0'));
  BOOST_CHECK_EQUAL("first cut", f.ReadLine('\0'));
  BOOST_CHECK_EQUAL("this", f.ReadLine('\0'));
  BOOST_CHECK_EQUAL("a first", f.ReadLine('\0'));
  BOOST_CHECK_EQUAL("cut", f.ReadLine('\0'));
  BOOST_CHECK_EQUAL("is", f.ReadLine('\0'));
  BOOST_CHECK_EQUAL("i", f.ReadLine('\0'));
  BOOST_CHECK_EQUAL("secd", f.ReadLine('\0'));
  BOOST_CHECK_EQUAL("first", f.ReadLine('\0'));
  BOOST_CHECK_THROW(f.ReadLine('\0'), util::EndOfFileException);
}

BOOST_AUTO_TEST_CASE(MergeVocabNoUnkTest) {
  TestFiles files;
  util::FixedArray<util::scoped_fd> used_files(1);
  used_files.push_back(files.no_unk.release());

  std::vector<lm::WordIndex> model_max_idx;
  model_max_idx.push_back(10);

  UniversalVocab universal_vocab(model_max_idx);
  util::scoped_fd combined(util::MakeTemp("temporary"));
  BOOST_CHECK_THROW(MergeVocab(used_files, universal_vocab, combined.get()), FormatLoadException);
}

BOOST_AUTO_TEST_CASE(MergeVocabWrongOrderTest) {
  TestFiles files;

  util::FixedArray<util::scoped_fd> used_files(2);
  used_files.push_back(files.test[0].release());
  used_files.push_back(files.bad_order.release());

  std::vector<lm::WordIndex> model_max_idx;
  model_max_idx.push_back(10);
  model_max_idx.push_back(10);

  lm::interpolate::UniversalVocab universal_vocab(model_max_idx);
  util::scoped_fd combined(util::MakeTemp("temporary"));
  BOOST_CHECK_THROW(MergeVocab(used_files, universal_vocab, combined.get()), FormatLoadException);
}

}}} // namespaces
