# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2017 Intel Corporation

# DPDK サンプル - Meson ビルド設定

project('dpdk-app', 'c',
    version: '1.0.0',
    default_options: [
        'c_std=c11',
        'warning_level=2',
    ]
)

# DPDK の pkg-config による検出
dpdk = dependency('libdpdk', required: true)

# デバッグ用マクロの有効化
add_project_arguments('-DRTE_LIBRTE_MBUF_DEBUG', language: 'c')

# 実行ファイルのビルド (統合版)
executable('dpdk-app',
    sources: 'main.c',
    dependencies: dpdk,
    install: true,
)
