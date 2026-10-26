// Stand-alone test of MapDocument / the command stack against a plain-array model (no engine needed).
//   build + run: python scripts/ed_native_tests.py
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "moho/editor/MapDocument.h"

using moho::editor::CellRect;
using moho::editor::MapDocument;

static int gFailures = 0;
#define CHECK(cond, ...)                               \
  do {                                                 \
    if (!(cond)) {                                     \
      ++gFailures;                                     \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);                        \
      std::printf("\n");                               \
    }                                                  \
  } while (0)

// The model keeps one full copy of the heights per stack position: states[i] = heights with i commands applied.
struct Harness
{
  int w, h;
  MapDocument doc;
  std::vector<std::uint16_t> mirror;               // what the sink saw: must always equal the document
  std::vector<std::vector<std::uint16_t>> states;
  std::size_t pos = 0;

  Harness(const int width, const int height, std::mt19937& rng) : w(width), h(height)
  {
    std::vector<std::uint16_t> start(static_cast<std::size_t>(w) * h);
    for (auto& v : start) {
      v = static_cast<std::uint16_t>(rng() % 65536);
    }
    mirror = start;
    states.push_back(start);
    doc.Reset(w, h, start, [this](const CellRect& r, const std::uint16_t* words) {
      std::size_t i = 0;
      for (int z = r.z0; z <= r.z1; ++z) {
        for (int x = r.x0; x <= r.x1; ++x, ++i) {
          mirror[static_cast<std::size_t>(z) * this->w + x] = words[i];
        }
      }
    });
  }

  std::vector<std::uint16_t> Current() const
  {
    std::vector<std::uint16_t> out(static_cast<std::size_t>(w) * h);
    for (int z = 0; z < h; ++z) {
      for (int x = 0; x < w; ++x) {
        out[static_cast<std::size_t>(z) * w + x] = doc.HeightAt(x, z);
      }
    }
    return out;
  }

  void Verify(const char* what)
  {
    const auto cur = Current();
    CHECK(cur == states[pos], "%s: document differs from the model at stack position %zu", what, pos);
    CHECK(mirror == cur, "%s: sink projection differs from the document", what);
    CHECK(doc.UndoDepth() == pos, "%s: undo depth %zu, model %zu", what, doc.UndoDepth(), pos);
    CHECK(doc.RedoDepth() == states.size() - 1 - pos, "%s: redo depth", what);
  }

  // One edit through the document. `newCommand` tells the model whether it opens a new stack position (a new
  // command) or merges into the top command of an open stroke.
  void Apply(const CellRect& r, const std::vector<std::uint16_t>& words, const bool newCommand)
  {
    const std::size_t changed = doc.EditHeights(r, words.data());
    if (changed == 0) {
      return;
    }
    if (newCommand) {
      states.resize(pos + 1);   // the redo branch is gone
      states.push_back(Current());
      pos = states.size() - 1;
    } else {
      states[pos] = Current();
    }
  }

  std::vector<std::uint16_t> RandomWords(std::mt19937& rng, const CellRect& r, const bool extreme)
  {
    std::vector<std::uint16_t> words;
    doc.ReadRect(r, words);
    for (auto& v : words) {
      const unsigned roll = rng() % 4;
      if (extreme) {
        v = (rng() & 1) ? 0 : 65535;
      } else if (roll == 0) {
        v = static_cast<std::uint16_t>(rng() % 65536);
      } else if (roll == 1) {
        v = static_cast<std::uint16_t>(std::min<int>(65535, v + static_cast<int>(rng() % 300)));
      }
    }
    return words;
  }

  CellRect RandomRect(std::mt19937& rng)
  {
    CellRect r;
    r.x0 = static_cast<int>(rng() % w);
    r.z0 = static_cast<int>(rng() % h);
    r.x1 = std::min(w - 1, r.x0 + static_cast<int>(rng() % 40));
    r.z1 = std::min(h - 1, r.z0 + static_cast<int>(rng() % 40));
    return r;
  }
};

int main()
{
  std::mt19937 rng(12345);

  // 1. random single edits, strokes, undo and redo against the model
  {
    Harness t(150, 120, rng);
    bool strokeOpen = false;
    bool strokeHasCommand = false;
    const auto endStroke = [&]() {
      if (strokeOpen) {
        t.doc.EndStroke();
        strokeOpen = false;
        strokeHasCommand = false;
      }
    };
    for (int step = 0; step < 2500; ++step) {
      const unsigned op = rng() % 10;
      if (op < 5) {
        if (!strokeOpen && rng() % 3 == 0) {
          t.doc.BeginStroke("stroke");
          strokeOpen = true;
        }
        const CellRect r = t.RandomRect(rng);
        const auto words = t.RandomWords(rng, r, rng() % 7 == 0);
        const std::size_t depth = t.doc.UndoDepth();
        const bool newCommand = !(strokeOpen && strokeHasCommand);
        t.Apply(r, words, newCommand);
        if (strokeOpen && t.doc.UndoDepth() != depth) {
          strokeHasCommand = true;
        }
      } else if (op < 6) {
        endStroke();
      } else if (op < 8) {
        endStroke();
        const auto result = t.doc.Undo();
        if (t.pos > 0) {
          CHECK(result.ok && result.verified, "undo not verified at step %d", step);
          --t.pos;
        } else {
          CHECK(!result.ok, "undo at the bottom must report nothing");
        }
      } else {
        endStroke();
        const auto result = t.doc.Redo();
        if (t.pos + 1 < t.states.size()) {
          CHECK(result.ok && result.verified, "redo not verified at step %d", step);
          ++t.pos;
        } else {
          CHECK(!result.ok, "redo at the top must report nothing");
        }
      }
      t.Verify("random walk");
    }
    endStroke();
    // undo everything, then redo everything: exact by checksum all the way
    while (t.doc.UndoDepth() > 0) {
      CHECK(t.doc.Undo().verified, "undo to the bottom: checksum mismatch");
      --t.pos;
    }
    t.Verify("fully undone");
    CHECK(t.Current() == t.states[0], "fully undone differs from the start");
    while (t.doc.RedoDepth() > 0) {
      CHECK(t.doc.Redo().verified, "redo to the top: checksum mismatch");
      ++t.pos;
    }
    t.Verify("fully redone");
  }

  // 2. dirty tracking
  {
    Harness t(64, 64, rng);
    const auto edit = [&]() {
      const CellRect r = t.RandomRect(rng);
      auto words = t.RandomWords(rng, r, true);
      t.Apply(r, words, true);
    };
    CHECK(!t.doc.Dirty(), "fresh document is clean");
    edit();
    edit();
    CHECK(t.doc.Dirty(), "dirty after edits");
    t.doc.MarkClean();
    CHECK(!t.doc.Dirty(), "clean after MarkClean");
    t.doc.Undo();
    CHECK(t.doc.Dirty(), "dirty after undo past the clean point");
    t.doc.Redo();
    CHECK(!t.doc.Dirty(), "clean again after redo to the clean point");
    t.doc.Undo();
    std::vector<std::uint16_t> w(1, 777);
    t.doc.EditHeights(CellRect{3, 3, 3, 3}, w.data());   // new edit drops the redo branch holding the clean state
    CHECK(t.doc.Dirty(), "dirty: the clean state is gone with the redo branch");
    t.doc.MarkClean();
    t.doc.ClearHistory();
    CHECK(!t.doc.Dirty(), "clear history of a clean document stays clean");
    w[0] = 778;
    t.doc.EditHeights(CellRect{4, 4, 4, 4}, w.data());
    t.doc.ClearHistory();
    CHECK(t.doc.Dirty(), "clear history of a dirty document stays dirty");
    std::vector<std::uint16_t> ext(4, 9);
    t.doc.MarkClean();
    t.doc.WriteExternal(CellRect{0, 0, 1, 1}, ext.data());
    CHECK(t.doc.Dirty() && t.doc.UndoDepth() == 0, "external write: dirty, history gone");
  }

  // 3. trim: more than kMaxCommands edits keep the newest ones and stay exact
  {
    Harness t(40, 40, rng);
    std::vector<std::vector<std::uint16_t>> history;
    history.push_back(t.Current());
    const int count = static_cast<int>(MapDocument::kMaxCommands) + 150;
    for (int i = 0; i < count; ++i) {
      std::vector<std::uint16_t> word(1, static_cast<std::uint16_t>(i + 1));
      const int x = i % 40;
      const int z = (i / 40) % 40;
      t.doc.EditHeights(CellRect{x, z, x, z}, word.data());
      history.push_back(t.Current());
    }
    CHECK(t.doc.UndoDepth() == MapDocument::kMaxCommands, "trim kept %zu commands", t.doc.UndoDepth());
    for (std::size_t i = 0; i < MapDocument::kMaxCommands; ++i) {
      CHECK(t.doc.Undo().verified, "trimmed history undo mismatch");
    }
    CHECK(t.Current() == history[150], "undoing the whole trimmed history lands on the state 150 edits in");
  }

  // 4. a stroke across tile borders is one undo step; labels
  {
    Harness t(200, 200, rng);
    t.doc.BeginStroke("raise");
    for (int i = 0; i < 40; ++i) {
      const CellRect r{i * 4, i * 4, i * 4 + 11, i * 4 + 11};
      std::vector<std::uint16_t> words;
      t.doc.ReadRect(r, words);
      for (auto& v : words) {
        v = static_cast<std::uint16_t>(std::min<int>(65535, v + 50));
      }
      t.doc.EditHeights(r, words.data());
    }
    t.doc.EndStroke();
    CHECK(t.doc.UndoDepth() == 1, "40 dabs of one stroke are one undo step, got %zu", t.doc.UndoDepth());
    CHECK(t.doc.UndoLabel() == "raise", "label");
    CHECK(t.doc.Undo().verified, "stroke undo verified");
    CHECK(t.Current() == t.states[0], "stroke undo restores the start");
    CHECK(t.doc.RedoLabel() == "raise", "redo label");
    CHECK(t.doc.Redo().verified, "stroke redo verified");
  }

  if (gFailures == 0) {
    std::printf("PASS MapDocumentTest\n");
    return 0;
  }
  std::printf("%d failure(s)\n", gFailures);
  return 1;
}
