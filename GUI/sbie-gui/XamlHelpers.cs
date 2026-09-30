// ---------------------------------------------------------------------------
// sbie-gui - Sandboxie-OSS WinUI 3 management GUI (thin shell over sbie-cli.exe)
// Copyright (C) 2026 Sandboxie-OSS contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// XAML 绑定辅助（x:Bind 静态函数）。Models 保持 UI 无关，
// bool->Visibility 的"取反"在视图层完成。
// ---------------------------------------------------------------------------

using Microsoft.UI.Xaml;

namespace sbie_gui;

public static class XamlHelpers
{
    public static Visibility VisibleWhen(bool b) => b ? Visibility.Visible : Visibility.Collapsed;

    public static Visibility CollapsedWhen(bool b) => b ? Visibility.Collapsed : Visibility.Visible;
}
