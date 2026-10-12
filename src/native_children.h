#pragma once
#include "ttp_skin_plugin.h"
#include <commctrl.h>
#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

namespace waskin {
// A provider owns the surface, but the host keeps its native HWNDs alive.
// Hiding once (or on a timer) is insufficient: ShowWindow, WM_SETREDRAW and
// a newly created child can expose the native skin between provider frames.
class NativeChildren {
public:
    NativeChildren() = default;
    NativeChildren(const NativeChildren&) = delete;
    NativeChildren& operator=(const NativeChildren&) = delete;
    ~NativeChildren() { Detach(); }

    void Attach(HWND parent) { Detach(); parent_ = parent; Sync(); }
    void Sync(HWND created = nullptr) {
        if (!IsWindow(parent_)) return;
        for (HWND child = GetWindow(parent_, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            if (!Blocked(child)) continue;
            const auto found = std::find_if(children_.begin(), children_.end(),
                [child](const auto& entry) { return entry.window == child; });
            if (found == children_.end()) {
                const bool visible = (GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE) != 0;
                children_.push_back({child, visible, child == created});
                if (!SetWindowSubclass(child, Subclass, id, reinterpret_cast<DWORD_PTR>(this))) {
                    children_.pop_back();
                    throw std::runtime_error("native child subclass");
                }
            }
            if (GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE) {
                const bool previous = std::exchange(hiding_, true);
                ShowWindow(child, SW_HIDE);
                hiding_ = previous;
            }
        }
    }
    void Detach() noexcept {
        const HWND parent = std::exchange(parent_, nullptr);
        auto children = std::move(children_);
        children_.clear();
        for (const auto& [child, visible, created] : children) {
            (void)created;
            DWORD_PTR owner{};
            if (!GetWindowSubclass(child, Subclass, id, &owner) || owner != reinterpret_cast<DWORD_PTR>(this)) continue;
            RemoveWindowSubclass(child, Subclass, id);
            // Editors are host-managed and may have been tagged/reparented
            // after WM_PARENTNOTIFY. Never resurrect a cached editor toolbar.
            if (visible && GetParent(child) == parent && !GetPropW(child, TTP_SKIN_CONTENT_CHILD))
                SetWindowPos(child, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE |
                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_SHOWWINDOW);
        }
    }
private:
    static constexpr UINT_PTR id = 0x77534b43;
    HWND parent_{};
    bool hiding_{};
    struct Entry { HWND window; bool visible, created; };
    std::vector<Entry> children_;
    bool Blocked(HWND child) const {
        // Detached fullscreen lyrics/visuals and embedded editor content are
        // intentional host surfaces, not native chrome belonging to this skin.
        return parent_ && GetParent(child) == parent_ &&
            (GetWindowLongPtrW(child, GWL_STYLE) & WS_CHILD) &&
            !GetPropW(child, TTP_SKIN_CONTENT_CHILD);
    }
    static LRESULT CALLBACK Subclass(HWND child, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
        auto& self = *reinterpret_cast<NativeChildren*>(data);
        if (message == WM_NCDESTROY) {
            RemoveWindowSubclass(child, Subclass, id);
            auto& entries = self.children_;
            entries.erase(std::remove_if(entries.begin(), entries.end(),
                [child](const auto& entry) { return entry.window == child; }), entries.end());
        } else if (self.Blocked(child)) {
            switch (message) {
            case WM_SHOWWINDOW:
                // User32 clears WS_VISIBLE during CreateWindowEx, then shows
                // the child after WM_PARENTNOTIFY. Retain that creation intent
                // so a lazily created native control can return on detach.
                for (auto& entry : self.children_)
                    if (entry.window == child && entry.created && !self.hiding_) entry.visible = wp != 0;
                break;
            case WM_WINDOWPOSCHANGING: {
                auto& position = *reinterpret_cast<WINDOWPOS*>(lp);
                position.flags &= ~SWP_SHOWWINDOW;
                position.flags |= SWP_HIDEWINDOW;
                break;
            }
            case WM_STYLECHANGING:
                if (static_cast<int>(wp) == GWL_STYLE)
                    reinterpret_cast<STYLESTRUCT*>(lp)->styleNew &= ~WS_VISIBLE;
                break;
            case WM_SETREDRAW: return 0;
            case WM_PAINT: {
                // Begin/End also consume internal paint requests. Merely
                // validating the rectangle can leave WM_PAINT in the queue.
                PAINTSTRUCT paint{};
                BeginPaint(child, &paint); EndPaint(child, &paint); return 0;
            }
            case WM_PRINT:
            case WM_PRINTCLIENT: return 0;
            case WM_ERASEBKGND: return 1;
            }
        }
        return DefSubclassProc(child, message, wp, lp);
    }
};
}
