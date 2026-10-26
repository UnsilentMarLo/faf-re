// MapDocument and its command stack, see MapDocument.h. Engine independent.
#include "moho/editor/MapDocument.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <utility>

namespace moho::editor
{
  void CellRect::Unite(const CellRect& other) noexcept
  {
    if (other.Empty()) {
      return;
    }
    if (Empty()) {
      *this = other;
      return;
    }
    x0 = std::min(x0, other.x0);
    z0 = std::min(z0, other.z0);
    x1 = std::max(x1, other.x1);
    z1 = std::max(z1, other.z1);
  }

  namespace
  {
    constexpr int kTile = 32;
    constexpr int kTileCells = kTile * kTile;

    [[nodiscard]] std::uint64_t TileKey(const int tx, const int tz) noexcept
    {
      return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(tz)) << 32) | static_cast<std::uint32_t>(tx);
    }

    // ---- delta packing: zero runs and zigzag varints -------------------------------------------------------

    void PutVarint(std::vector<std::uint8_t>& out, std::uint32_t value)
    {
      while (value >= 0x80u) {
        out.push_back(static_cast<std::uint8_t>(value | 0x80u));
        value >>= 7;
      }
      out.push_back(static_cast<std::uint8_t>(value));
    }

    [[nodiscard]] std::uint32_t GetVarint(const std::vector<std::uint8_t>& in, std::size_t& at)
    {
      std::uint32_t value = 0;
      int shift = 0;
      while (at < in.size()) {
        const std::uint8_t byte = in[at++];
        value |= static_cast<std::uint32_t>(byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0) {
          break;
        }
        shift += 7;
      }
      return value;
    }

    [[nodiscard]] std::uint32_t ZigZag(const std::int32_t v) noexcept
    {
      return (static_cast<std::uint32_t>(v) << 1) ^ static_cast<std::uint32_t>(v >> 31);
    }

    [[nodiscard]] std::int32_t UnZigZag(const std::uint32_t v) noexcept
    {
      return static_cast<std::int32_t>(v >> 1) ^ -static_cast<std::int32_t>(v & 1u);
    }

    // token 0 + run length = a run of zeros; any other varint is a zigzag-coded non-zero delta
    std::vector<std::uint8_t> Pack(const std::vector<std::int32_t>& cells)
    {
      std::vector<std::uint8_t> out;
      std::uint32_t run = 0;
      const auto flush = [&]() {
        if (run != 0) {
          PutVarint(out, 0);
          PutVarint(out, run);
          run = 0;
        }
      };
      for (const std::int32_t d : cells) {
        if (d == 0) {
          ++run;
        } else {
          flush();
          PutVarint(out, ZigZag(d));
        }
      }
      flush();
      out.shrink_to_fit();
      return out;
    }

    void Unpack(const std::vector<std::uint8_t>& in, std::vector<std::int32_t>& cells)
    {
      cells.assign(static_cast<std::size_t>(kTileCells), 0);
      std::size_t at = 0;
      std::size_t cell = 0;
      while (at < in.size() && cell < cells.size()) {
        const std::uint32_t token = GetVarint(in, at);
        if (token == 0) {
          cell += GetVarint(in, at);
        } else {
          cells[cell++] = UnZigZag(token);
        }
      }
    }

    // ---- the height command -------------------------------------------------------------------------------

    /**
     * All dabs of one brush stroke (or one single edit): sparse per-tile deltas `after - before`. Undo needs no copy
     * of the old samples because the stack is strictly last-in first-out, and the checksums prove it.
     */
    class HeightStroke final : public Command
    {
    public:
      HeightStroke(const std::uint64_t stroke, std::string label) : mStroke(stroke), mLabel(std::move(label)) {}

      /** `delta` is `after - before` for `rect`, row major. */
      void AddDab(const CellRect& rect, const std::vector<std::int32_t>& delta)
      {
        mBounds.Unite(rect);
        std::size_t i = 0;
        // consecutive samples of a row mostly share a tile: look the tile up in the map only when it changes (a dab of radius 50 has ~8000 changed
        // samples in ~9 tiles; one map lookup per sample was a measurable part of a brush dab)
        Tile* tile = nullptr;
        int tileX = -1;
        int tileZ = -1;
        for (int z = rect.z0; z <= rect.z1; ++z) {
          for (int x = rect.x0; x <= rect.x1; ++x, ++i) {
            const std::int32_t d = delta[i];
            if (d == 0) {
              continue;
            }
            if (tile == nullptr || x / kTile != tileX || z / kTile != tileZ) {
              tileX = x / kTile;
              tileZ = z / kTile;
              tile = &mTiles[TileKey(tileX, tileZ)];
              if (tile->raw.empty()) {
                tile->raw.assign(static_cast<std::size_t>(kTileCells), 0);
              }
            }
            tile->raw[static_cast<std::size_t>((z % kTile) * kTile + (x % kTile))] += d;
          }
        }
      }

      void Do(MapDocument& doc) override { (void)Apply(doc, +1); }

      bool Redo(MapDocument& doc) override
      {
        bool ok = true;
        if (mSealed) {
          ok = doc.Checksum(mBounds) == mSumBefore;
        }
        Apply(doc, +1);
        if (mSealed) {
          ok = ok && doc.Checksum(mBounds) == mSumAfter;
        }
        return ok;
      }

      bool Undo(MapDocument& doc) override
      {
        bool ok = true;
        if (mSealed) {
          ok = doc.Checksum(mBounds) == mSumAfter;
        }
        Apply(doc, -1);
        if (mSealed) {
          ok = ok && doc.Checksum(mBounds) == mSumBefore;
        }
        return ok;
      }

      bool TryMerge(Command& next) override
      {
        auto* const other = dynamic_cast<HeightStroke*>(&next);
        if (other == nullptr || mSealed || other->mSealed || mStroke == 0 || other->mStroke != mStroke) {
          return false;
        }
        for (auto& [key, tile] : other->mTiles) {
          Tile& mine = mTiles[key];
          if (mine.raw.empty()) {
            mine.raw = std::move(tile.raw);
          } else {
            for (std::size_t i = 0; i < mine.raw.size(); ++i) {
              mine.raw[i] += tile.raw[i];
            }
          }
        }
        mBounds.Unite(other->mBounds);
        return true;
      }

      void Seal(MapDocument& doc) override
      {
        if (mSealed) {
          return;
        }
        // the document is in the state after this command: record it, and derive the state before it
        mSumAfter = doc.Checksum(mBounds);
        std::vector<std::uint16_t> before;
        doc.ReadRect(mBounds, before);
        Accumulate(before, -1);
        mSumBefore = MapDocument::ChecksumWords(mBounds, before.data());
        for (auto& [key, tile] : mTiles) {
          tile.packed = Pack(tile.raw);
          tile.raw.clear();
          tile.raw.shrink_to_fit();
        }
        mSealed = true;
      }

      [[nodiscard]] std::string Label() const override { return mLabel; }
      [[nodiscard]] CellRect Bounds() const override { return mBounds; }
      [[nodiscard]] std::uint64_t StrokeId() const noexcept override { return mStroke; }

      [[nodiscard]] std::size_t ApproxBytes() const override
      {
        std::size_t total = sizeof(*this) + mLabel.size();
        for (const auto& [key, tile] : mTiles) {
          (void)key;
          total += tile.raw.size() * sizeof(std::int32_t) + tile.packed.size() + 48;
        }
        return total;
      }

    private:
      struct Tile
      {
        std::vector<std::int32_t> raw;     // open stroke
        std::vector<std::uint8_t> packed;  // sealed
      };

      // buffer = the document's words over mBounds; adds sign * delta to it
      void Accumulate(std::vector<std::uint16_t>& buffer, const int sign) const
      {
        const int width = mBounds.Width();
        std::vector<std::int32_t> cells;
        for (const auto& [key, tile] : mTiles) {
          const int tx = static_cast<int>(key & 0xFFFFFFFFu);
          const int tz = static_cast<int>(key >> 32);
          const std::vector<std::int32_t>* source = &tile.raw;
          if (mSealed) {
            Unpack(tile.packed, cells);
            source = &cells;
          }
          if (source->empty()) {
            continue;
          }
          for (int cz = 0; cz < kTile; ++cz) {
            const int z = tz * kTile + cz;
            if (z < mBounds.z0 || z > mBounds.z1) {
              continue;
            }
            for (int cx = 0; cx < kTile; ++cx) {
              const int x = tx * kTile + cx;
              if (x < mBounds.x0 || x > mBounds.x1) {
                continue;
              }
              const std::int32_t d = (*source)[static_cast<std::size_t>(cz * kTile + cx)];
              if (d == 0) {
                continue;
              }
              std::uint16_t& word = buffer[static_cast<std::size_t>((z - mBounds.z0) * width + (x - mBounds.x0))];
              const std::int32_t v = static_cast<std::int32_t>(word) + sign * d;
              word = static_cast<std::uint16_t>(std::clamp(v, 0, 65535));
            }
          }
        }
      }

      bool Apply(MapDocument& doc, const int sign)
      {
        if (mBounds.Empty()) {
          return true;
        }
        std::vector<std::uint16_t> buffer;
        doc.ReadRect(mBounds, buffer);
        Accumulate(buffer, sign);
        doc.CommitHeights(mBounds, buffer.data());
        return true;
      }

      std::map<std::uint64_t, Tile> mTiles;
      CellRect mBounds;
      std::uint64_t mStroke = 0;
      std::string mLabel;
      bool mSealed = false;
      std::uint64_t mSumBefore = 0;
      std::uint64_t mSumAfter = 0;
    };
  } // namespace

  // ---- MapDocument ---------------------------------------------------------------------------------------------

  MapDocument::MapDocument() = default;
  MapDocument::~MapDocument() = default;

  void MapDocument::Reset(const int width, const int height, std::vector<std::uint16_t> heights, HeightSink sink)
  {
    mStack.clear();
    mPos = 0;
    mCleanPos = 0;
    mStroke = 0;
    mRevision = 0;
    mSink = std::move(sink);
    if (width <= 0 || height <= 0 || heights.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {
      mWidth = 0;
      mHeight = 0;
      mHeights.clear();
      return;
    }
    mWidth = width;
    mHeight = height;
    mHeights = std::move(heights);
  }

  std::uint16_t MapDocument::HeightAt(const int x, const int z) const
  {
    if (x < 0 || z < 0 || x >= mWidth || z >= mHeight) {
      return 0;
    }
    return mHeights[static_cast<std::size_t>(z) * static_cast<std::size_t>(mWidth) + static_cast<std::size_t>(x)];
  }

  CellRect MapDocument::Clamp(CellRect rect) const noexcept
  {
    if (rect.x0 > rect.x1) {
      std::swap(rect.x0, rect.x1);
    }
    if (rect.z0 > rect.z1) {
      std::swap(rect.z0, rect.z1);
    }
    rect.x0 = std::max(0, rect.x0);
    rect.z0 = std::max(0, rect.z0);
    rect.x1 = std::min(mWidth - 1, rect.x1);
    rect.z1 = std::min(mHeight - 1, rect.z1);
    return rect;
  }

  void MapDocument::ReadRect(const CellRect& rect, std::vector<std::uint16_t>& out) const
  {
    out.clear();
    if (rect.Empty() || rect.x0 < 0 || rect.z0 < 0 || rect.x1 >= mWidth || rect.z1 >= mHeight) {
      return;
    }
    out.resize(rect.Count());
    const std::size_t rowWords = static_cast<std::size_t>(rect.Width());
    for (int z = rect.z0; z <= rect.z1; ++z) {
      std::memcpy(
        out.data() + static_cast<std::size_t>(z - rect.z0) * rowWords,
        mHeights.data() + static_cast<std::size_t>(z) * static_cast<std::size_t>(mWidth) + static_cast<std::size_t>(rect.x0),
        rowWords * sizeof(std::uint16_t)
      );
    }
  }

  std::uint64_t MapDocument::ChecksumWords(const CellRect& rect, const std::uint16_t* const words)
  {
    std::uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](const std::uint64_t value) {
      hash ^= value;
      hash *= 1099511628211ull;
    };
    mix(static_cast<std::uint64_t>(rect.Width()));
    mix(static_cast<std::uint64_t>(rect.Height()));
    const std::size_t count = rect.Count();
    for (std::size_t i = 0; i < count; ++i) {
      mix(words[i]);
    }
    return hash;
  }

  std::uint64_t MapDocument::Checksum(const CellRect& rect) const
  {
    std::vector<std::uint16_t> words;
    ReadRect(rect, words);
    return ChecksumWords(rect, words.data());
  }

  void MapDocument::CommitHeights(const CellRect& rect, const std::uint16_t* const words)
  {
    if (rect.Empty() || rect.x0 < 0 || rect.z0 < 0 || rect.x1 >= mWidth || rect.z1 >= mHeight) {
      return;
    }
    const std::size_t rowWords = static_cast<std::size_t>(rect.Width());
    for (int z = rect.z0; z <= rect.z1; ++z) {
      std::memcpy(
        mHeights.data() + static_cast<std::size_t>(z) * static_cast<std::size_t>(mWidth) + static_cast<std::size_t>(rect.x0),
        words + static_cast<std::size_t>(z - rect.z0) * rowWords,
        rowWords * sizeof(std::uint16_t)
      );
    }
    ++mRevision;
    if (mSink) {
      mSink(rect, words);
    }
  }

  void MapDocument::WriteExternal(const CellRect& rect, const std::uint16_t* const words)
  {
    CommitHeights(rect, words);
    mStack.clear();
    mPos = 0;
    mStroke = 0;
    mCleanPos = -1;
  }

  std::size_t MapDocument::EditHeights(const CellRect& rect, const std::uint16_t* const words)
  {
    if (rect.Empty() || rect.x0 < 0 || rect.z0 < 0 || rect.x1 >= mWidth || rect.z1 >= mHeight) {
      return 0;
    }
    std::vector<std::int32_t> delta(rect.Count());
    std::size_t changed = 0;
    std::size_t i = 0;
    for (int z = rect.z0; z <= rect.z1; ++z) {
      for (int x = rect.x0; x <= rect.x1; ++x, ++i) {
        const std::int32_t d = static_cast<std::int32_t>(words[i]) - static_cast<std::int32_t>(HeightAt(x, z));
        delta[i] = d;
        changed += d != 0 ? 1u : 0u;
      }
    }
    if (changed == 0) {
      return 0;
    }
    auto command = std::make_unique<HeightStroke>(mStroke, mStroke != 0 ? mStrokeLabel : std::string("Height edit"));
    command->AddDab(rect, delta);
    Execute(std::move(command));
    return changed;
  }

  void MapDocument::Execute(std::unique_ptr<Command> command)
  {
    command->Do(*this);

    if (mPos < mStack.size()) {
      // a new edit ends the redo branch
      if (mCleanPos > static_cast<long>(mPos)) {
        mCleanPos = -1;
      }
      mStack.resize(mPos);
    }
    if (mPos > 0 && mStroke != 0 && mStack[mPos - 1]->StrokeId() == mStroke && mStack[mPos - 1]->TryMerge(*command)) {
      return;
    }
    Push(std::move(command));
  }

  void MapDocument::Push(std::unique_ptr<Command> command)
  {
    mStack.push_back(std::move(command));
    ++mPos;
    if (mStroke == 0) {
      mStack.back()->Seal(*this);
    }
    Trim();
  }

  void MapDocument::Trim()
  {
    while (mStack.size() > 1 && (mStack.size() > kMaxCommands || HistoryBytes() > kMaxBytes)) {
      mStack.erase(mStack.begin());
      if (mPos > 0) {
        --mPos;
      }
      if (mCleanPos >= 0) {
        --mCleanPos;
        if (mCleanPos < 0) {
          mCleanPos = -1;
        }
      }
    }
  }

  void MapDocument::BeginStroke(const std::string& label)
  {
    if (mStroke != 0) {
      return;
    }
    mStroke = mNextStroke++;
    mStrokeLabel = label;
  }

  void MapDocument::EndStroke()
  {
    if (mStroke == 0) {
      return;
    }
    if (mPos > 0 && mPos == mStack.size() && mStack[mPos - 1]->StrokeId() == mStroke) {
      mStack[mPos - 1]->Seal(*this);
    }
    mStroke = 0;
  }

  HistoryResult MapDocument::Undo()
  {
    EndStroke();
    HistoryResult result;
    if (mPos == 0) {
      return result;
    }
    Command& command = *mStack[mPos - 1];
    result.label = command.Label();
    result.bounds = command.Bounds();
    result.verified = command.Undo(*this);
    result.ok = true;
    --mPos;
    return result;
  }

  HistoryResult MapDocument::Redo()
  {
    EndStroke();
    HistoryResult result;
    if (mPos >= mStack.size()) {
      return result;
    }
    Command& command = *mStack[mPos];
    result.label = command.Label();
    result.bounds = command.Bounds();
    result.verified = command.Redo(*this);
    result.ok = true;
    ++mPos;
    return result;
  }

  std::string MapDocument::UndoLabel() const
  {
    return mPos > 0 ? mStack[mPos - 1]->Label() : std::string();
  }

  std::string MapDocument::RedoLabel() const
  {
    return mPos < mStack.size() ? mStack[mPos]->Label() : std::string();
  }

  std::size_t MapDocument::HistoryBytes() const
  {
    std::size_t total = 0;
    for (const auto& command : mStack) {
      total += command->ApproxBytes();
    }
    return total;
  }

  void MapDocument::ClearHistory()
  {
    const bool wasDirty = Dirty();
    mStack.clear();
    mPos = 0;
    mStroke = 0;
    mCleanPos = wasDirty ? -1 : 0;
  }
} // namespace moho::editor
