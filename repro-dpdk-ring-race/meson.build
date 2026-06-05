# SPDX-License-Identifier: BSD-3-Clause
# Copyright(c) 2026

project('dpdk-ring-race', 'c',
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

# MPリング選択マクロの追加
if get_option('use_mp')
    add_project_arguments('-DUSE_MP_RING', language: 'c')
endif

# バーストサイズ指定マクロの追加
burst_size = get_option('burst_size')
add_project_arguments('-DBURST_SIZE=@0@'.format(burst_size), language: 'c')


# 実行ファイルのビルド
executable('dpdk-app',
    sources: 'main.c',
    dependencies: dpdk,
    install: true,
)
