#ifndef LLVM_DEBUGINFO_DWARF_DWARFCONTEXT_H
#define LLVM_DEBUGINFO_DWARF_DWARFCONTEXT_H

#include <liric/llvm_compat_c.h>
#include "llvm/DebugInfo/Symbolize/Symbolize.h"
#include "llvm/Object/ObjectFile.h"
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace llvm {

class DWARFUnit {
    const lr_llvm_compat_dwarf_unit_t *Unit_ = nullptr;

public:
    explicit DWARFUnit(const lr_llvm_compat_dwarf_unit_t *Unit)
        : Unit_(Unit) {}

    const lr_llvm_compat_dwarf_unit_t *raw() const { return Unit_; }

    const char *getCompilationDir() const {
        return lr_llvm_compat_dwarf_unit_compilation_dir(Unit_);
    }
};

namespace DWARFDebugLine {

struct Row {
    bool EndSequence = false;
    uint64_t Line = 0;
    uint64_t File = 0;
    object::SectionedAddress Address = {};
};

class LineTable {
    const lr_llvm_compat_dwarf_context_t *Ctx_ = nullptr;
    const lr_llvm_compat_dwarf_unit_t *Unit_ = nullptr;

public:
    std::vector<Row> Rows;

    LineTable() = default;

    LineTable(const lr_llvm_compat_dwarf_context_t *Ctx,
              const lr_llvm_compat_dwarf_unit_t *Unit)
        : Ctx_(Ctx), Unit_(Unit) {
        size_t n = lr_llvm_compat_dwarf_line_row_count(Ctx_, Unit_);
        for (size_t i = 0; i < n; i++) {
            lr_llvm_compat_dwarf_row_t R;
            Row Out;
            if (lr_llvm_compat_dwarf_line_row_get(Ctx_, Unit_, i, &R) != 0)
                continue;
            Out.EndSequence = (R.end_sequence != 0);
            Out.Line = R.line;
            Out.File = R.file;
            Out.Address.Address = R.address;
            Out.Address.SectionIndex = R.section_index;
            Rows.push_back(Out);
        }
    }

    bool hasFileAtIndex(uint64_t Index) const {
        return lr_llvm_compat_dwarf_line_has_file_index(Ctx_, Unit_, Index) != 0;
    }

    bool getFileNameByIndex(uint64_t Index,
                            const char *CompDir,
                            DILineInfoSpecifier::FileLineInfoKind Kind,
                            std::string &Result) const {
        char Buf[4096];
        size_t n = 0;
        int rc = lr_llvm_compat_dwarf_line_get_file_name(
            Ctx_, Unit_, Index, CompDir ? CompDir : "",
            (int)Kind, Buf, sizeof(Buf), &n);
        if (rc != 0)
            return false;
        Result.assign(Buf, n);
        return true;
    }
};

} // namespace DWARFDebugLine

class DWARFContext {
    lr_llvm_compat_dwarf_context_t *Ctx_ = nullptr;
    std::vector<std::unique_ptr<DWARFUnit>> Units_;
    mutable std::unique_ptr<DWARFDebugLine::LineTable> LineTableCache_;

public:
    explicit DWARFContext(lr_llvm_compat_dwarf_context_t *Ctx)
        : Ctx_(Ctx) {
        size_t n = lr_llvm_compat_dwarf_context_unit_count(Ctx_);
        for (size_t i = 0; i < n; i++) {
            const lr_llvm_compat_dwarf_unit_t *U =
                lr_llvm_compat_dwarf_context_unit_at(Ctx_, i);
            if (!U)
                continue;
            Units_.push_back(std::make_unique<DWARFUnit>(U));
        }
    }

    ~DWARFContext() {
        if (Ctx_) {
            lr_llvm_compat_dwarf_context_destroy(Ctx_);
            Ctx_ = nullptr;
        }
    }

    DWARFContext(const DWARFContext &) = delete;
    DWARFContext &operator=(const DWARFContext &) = delete;

    DWARFContext(DWARFContext &&Other) noexcept
        : Ctx_(Other.Ctx_),
          Units_(std::move(Other.Units_)),
          LineTableCache_(std::move(Other.LineTableCache_)) {
        Other.Ctx_ = nullptr;
    }

    DWARFContext &operator=(DWARFContext &&Other) noexcept {
        if (this == &Other)
            return *this;
        if (Ctx_)
            lr_llvm_compat_dwarf_context_destroy(Ctx_);
        Ctx_ = Other.Ctx_;
        Units_ = std::move(Other.Units_);
        LineTableCache_ = std::move(Other.LineTableCache_);
        Other.Ctx_ = nullptr;
        return *this;
    }

    static std::unique_ptr<DWARFContext> create(const object::ObjectFile &Obj) {
        lr_llvm_compat_dwarf_context_t *Ctx = nullptr;
        if (lr_llvm_compat_dwarf_context_create(Obj.raw_handle(), &Ctx) != 0)
            return nullptr;
        return std::unique_ptr<DWARFContext>(new DWARFContext(Ctx));
    }

    const std::vector<std::unique_ptr<DWARFUnit>> &compile_units() const {
        return Units_;
    }

    const DWARFDebugLine::LineTable *getLineTableForUnit(const DWARFUnit *Unit) const {
        if (!Unit)
            return nullptr;
        LineTableCache_ = std::make_unique<DWARFDebugLine::LineTable>(Ctx_, Unit->raw());
        return LineTableCache_.get();
    }
};

} // namespace llvm

#endif
