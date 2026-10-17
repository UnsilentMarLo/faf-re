// The editor's map document and its command stack (M1, docs/projects/MAP_EDITOR_DESIGN.md rules 1 and 2).
//
// MapDocument owns the truth of everything the editor edits (so far the height samples); the engine's height
// fields, the sim's path caches and the renderer are projections refreshed through one sink. Every edit is a
// Command on one stack: Do / Undo / TryMerge, a label, a dirty rectangle. Height payloads are stored as sparse
// per-tile deltas that are packed (zero-run + zigzag varint) when the stroke closes, and carry before / after
// checksums so undo can prove it restored the exact samples.
//
// This file is engine independent (no moho types) so the stack can be tested on its own
// (editor/tests/MapDocumentTest.cpp). The engine side is EditorDocument.h.
//
// Thread rule: a MapDocument is used by one thread only (the UI thread). The sink hands the changed rectangle to the
// engine glue, which is the only place that touches the sim thread (a queue drained between sim beats).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace moho::editor
{
  /** Inclusive sample rectangle. Empty when x1 < x0 or z1 < z0. */
  struct CellRect
  {
    int x0 = 0;
    int z0 = 0;
    int x1 = -1;
    int z1 = -1;

    [[nodiscard]] bool Empty() const noexcept { return x1 < x0 || z1 < z0; }
    [[nodiscard]] int Width() const noexcept { return Empty() ? 0 : x1 - x0 + 1; }
    [[nodiscard]] int Height() const noexcept { return Empty() ? 0 : z1 - z0 + 1; }
    [[nodiscard]] std::size_t Count() const noexcept
    {
      return static_cast<std::size_t>(Width()) * static_cast<std::size_t>(Height());
    }
    void Unite(const CellRect& other) noexcept;
  };

  class MapDocument;

  /** One undoable edit. Commands are executed by MapDocument::Execute, never by the tools directly. */
  class Command
  {
  public:
    virtual ~Command() = default;

    /** Applies the edit to the document (first execution and every redo). */
    virtual void Do(MapDocument& doc) = 0;

    /** Takes the edit back. Returns false when the document no longer matched what the command expects. */
    virtual bool Undo(MapDocument& doc) = 0;

    /** Applies the edit again after an undo; false when the document did not match what the command recorded. */
    virtual bool Redo(MapDocument& doc)
    {
      Do(doc);
      return true;
    }

    /** The brush stroke this command belongs to (0 = none: never merged into). */
    [[nodiscard]] virtual std::uint64_t StrokeId() const noexcept { return 0; }

    /** Absorbs `next`, which was just executed on top of this command (a later dab of the same brush stroke). */
    virtual bool TryMerge(Command& next)
    {
      (void)next;
      return false;
    }

    /** Called when no more commands will be merged into this one: pack the payload, record checksums. */
    virtual void Seal(MapDocument& doc) { (void)doc; }

    [[nodiscard]] virtual std::string Label() const = 0;
    /** The samples this command changed (for re-snapping units, dirtying path caches ...). */
    [[nodiscard]] virtual CellRect Bounds() const = 0;
    [[nodiscard]] virtual std::size_t ApproxBytes() const = 0;
  };

  struct HistoryResult
  {
    bool ok = false;        // a command was undone / redone
    bool verified = true;   // checksums before and after matched what the command recorded
    CellRect bounds;        // what changed
    std::string label;
  };

  class MapDocument
  {
  public:
    /** Called after the document's heights changed: the rectangle and its words (row major, x fastest). */
    using HeightSink = std::function<void(const CellRect&, const std::uint16_t*)>;

    static constexpr std::size_t kMaxCommands = 2000;
    static constexpr std::size_t kMaxBytes = 256u * 1024u * 1024u;

    MapDocument();
    ~MapDocument();
    MapDocument(const MapDocument&) = delete;
    MapDocument& operator=(const MapDocument&) = delete;

    /** Starts a document from `heights` (width * height words); clears the history; the document is clean. */
    void Reset(int width, int height, std::vector<std::uint16_t> heights, HeightSink sink);

    [[nodiscard]] bool Valid() const noexcept { return mWidth > 0 && mHeight > 0; }
    [[nodiscard]] int Width() const noexcept { return mWidth; }
    [[nodiscard]] int Height() const noexcept { return mHeight; }
    [[nodiscard]] std::uint16_t HeightAt(int x, int z) const;
    [[nodiscard]] CellRect Clamp(CellRect rect) const noexcept;
    void ReadRect(const CellRect& rect, std::vector<std::uint16_t>& out) const;
    /** FNV-1a over the rectangle's words (the rectangle's size is part of the hash). */
    [[nodiscard]] std::uint64_t Checksum(const CellRect& rect) const;
    [[nodiscard]] static std::uint64_t ChecksumWords(const CellRect& rect, const std::uint16_t* words);

    // ---- writes ----------------------------------------------------------------------------------------------

    /** For commands only: writes words into the document and the sink, no history. */
    void CommitHeights(const CellRect& rect, const std::uint16_t* words);

    /** A write that did not come from a command (import, test): applied, history cleared, document dirty. */
    void WriteExternal(const CellRect& rect, const std::uint16_t* words);

    /**
     * The undoable height edit tools use: `words` is the new content of `rect` (row major). Samples that do not
     * change are not recorded; returns the number that did. Inside a stroke the edits merge into one command.
     */
    std::size_t EditHeights(const CellRect& rect, const std::uint16_t* words);

    /** Executes a command (Do, merge into the stack top or push). */
    void Execute(std::unique_ptr<Command> command);

    // ---- strokes ---------------------------------------------------------------------------------------------

    /** Edits until EndStroke form one undo step labelled `label`. Nested calls are ignored. */
    void BeginStroke(const std::string& label);
    void EndStroke();
    [[nodiscard]] bool StrokeOpen() const noexcept { return mStroke != 0; }

    // ---- history ---------------------------------------------------------------------------------------------

    HistoryResult Undo();
    HistoryResult Redo();
    [[nodiscard]] std::size_t UndoDepth() const noexcept { return mPos; }
    [[nodiscard]] std::size_t RedoDepth() const noexcept { return mStack.size() - mPos; }
    [[nodiscard]] std::string UndoLabel() const;
    [[nodiscard]] std::string RedoLabel() const;
    [[nodiscard]] std::size_t HistoryBytes() const;
    void ClearHistory();

    // ---- dirty tracking --------------------------------------------------------------------------------------

    /** True when the document differs from the last MarkClean (or the start). */
    [[nodiscard]] bool Dirty() const noexcept { return mCleanPos != static_cast<long>(mPos); }
    void MarkClean() noexcept { mCleanPos = static_cast<long>(mPos); }
    /** Increases with every change of the document (edit, undo, redo, external write). */
    [[nodiscard]] std::uint64_t Revision() const noexcept { return mRevision; }

    [[nodiscard]] std::uint64_t StrokeId() const noexcept { return mStroke; }

  private:
    void Push(std::unique_ptr<Command> command);
    void Trim();

    int mWidth = 0;
    int mHeight = 0;
    std::vector<std::uint16_t> mHeights;
    HeightSink mSink;

    std::vector<std::unique_ptr<Command>> mStack;
    std::size_t mPos = 0;   // commands applied; mStack[mPos..] is the redo branch
    long mCleanPos = 0;     // value of mPos at the last MarkClean; -1 when that state can no longer be reached
    std::uint64_t mRevision = 0;
    std::uint64_t mStroke = 0;
    std::uint64_t mNextStroke = 1;
    std::string mStrokeLabel;
  };
} // namespace moho::editor
