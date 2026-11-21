#!/usr/bin/env bash
# 门禁（fix-persistence-geometry 单变量）：只改 PicassoPersistence.h 守卫；
# identity 生成逐字节不变；4 个设备不符写死常量移除；structural guards 保留；
# DataInit.h 的 phase==2 resume 逻辑不动。
cd /d/k30i-gpu/repo-work || exit 1
F=drivers/NativeUfs/PicassoPersistence.h
D=drivers/NativeUfs/PicassoDataInit.h

# 1) identity 生成 9 行完整（bytes 0..87）
grep -qF 'put64(identity,gStoreGeometry.first)' "$F" &&
grep -qF 'put64(identity+8,gStoreGeometry.last)' "$F" &&
grep -qF 'put64(identity+16,gStoreGeometry.imageBytes)' "$F" &&
grep -qF 'put64(identity+24,gStoreRegionFirst)' "$F" &&
grep -qF 'put64(identity+32,gStoreRegionBlocks)' "$F" &&
grep -qF 'put32(identity+40,gStoreGeometry.headerCrc)' "$F" &&
grep -qF 'put32(identity+44,gStoreGeometry.entriesCrc)' "$F" &&
grep -qF 'put64(identity+48,pages)' "$F" &&
grep -qF 'memcpy(identity+56,seed,32)' "$F" &&
# 2) 4 个设备不符写死常量已移除
! grep -qF 'gStoreRegionFirst!=9825114' "$F" &&
! grep -qF 'gStoreGeometry.last!=15330298' "$F" &&
! grep -qF 'headerCrc!=0x3c17a352' "$F" &&
! grep -qF 'entriesCrc!=0xd82988c2' "$F" &&
# 3) structural guards 保留
grep -qF 'pages!=5242880' "$F" &&
grep -qF 'gStoreRegionBlocks!=5243041' "$F" &&
grep -qF 'gStoreGeometry.first!=2883584' "$F" &&
grep -qF 'imageBytes!=8380219392ULL' "$F" &&
grep -qF 'reserveBlocks()!=262144' "$F" &&
grep -qF 'native_ufs_store_gpt_check()' "$F" &&
grep -qF 'picassoRecordWindowOpen()' "$F" &&
# 4) resume 逻辑不动 + 修复本体等价判据（适配回退场景：不再断言"只改1个文件"）
git diff --quiet -- "$D" &&
grep -qF 'if(phase==2){complete=true;return 2;}' "$D" &&
# PicassoPersistence.h 相对 HEAD(f10634d) 无改动 → f10634d 的修复原样保留
git diff --quiet -- "$F" &&
git diff --check -- "$F"
echo PERSIST_GUARD_RC=$?
