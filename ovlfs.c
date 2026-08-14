/*
 * ovlfs.c - 高性能 WinFsp OverlayFS（upper 可写层 + 多个 lower 只读层）
 *
 * 语义:
 *   - 读: upper 优先, 其次 lower[0], lower[1] ...
 *   - 写: 首次以写方式打开时 copy-up 到 upper; 之后所有修改都在 upper
 *   - 删除/改名: lower 层条目用 upper 层中的 ":.<名字>" 隐藏 (whiteout)
 *   - 不实现: ACL/安全描述符(NULL DACL)、EA、命名流、重解析点、硬链接
 *
 * 编译 (VS 开发者命令行):
 *   cl /O2 /W3 /I "%ProgramFiles(x86)%\WinFsp\inc" ovlfs.c /link
 * /LIBPATH:"%ProgramFiles(x86)%\WinFsp\lib" winfsp-x64.lib
 *
 * 运行:
 *   ovlfs -u C:\ovl\upper -l C:\base1;C:\base2 -m O: [-i 1000] [-t 0] [-D 1]
 */

#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <winternl.h>
#pragma warning(push)
#pragma warning(disable : 4005)
#include <ntstatus.h>
#pragma warning(pop)
#include <winfsp/winfsp.h>

#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OVL_DISK_DEVICE_NAME L"WinFsp.Disk"
#define OVL_NET_DEVICE_NAME L"WinFsp.Net"

#define WHITEOUT_PREFIX L"\uF03A\uF02E"
#define WHITEOUT_PREFIX_LEN 2
#define COPY_BUF_SIZE (1024 * 1024)
#define ALLOC_UNIT ((UINT64)512 * 8)
#define MAX_LOWER_LAYERS 8

/* ------------------------------------------------------------------ */
/* 全局状态                                                            */
/* ------------------------------------------------------------------ */

static WCHAR g_UpperRoot[MAX_PATH];
static WCHAR g_LowerRoots[MAX_LOWER_LAYERS][MAX_PATH];
static ULONG g_LowerCount;
static WCHAR g_Label[32] = L"OVLFS";
static UINT64 g_AllocUnit = ALLOC_UNIT;
static HANDLE g_StopEvent;
static CRITICAL_SECTION g_CopyUpCs;

typedef struct _OVL_COPYUP {
  struct _OVL_COPYUP *Next;
  PWSTR Path;
  HANDLE Event;
  NTSTATUS Status;
  LONG Refs;
} OVL_COPYUP;
static OVL_COPYUP *g_CopyUps;

/* 每次打开实例一个描述符 (UmFileContextIsUserContext2) */
typedef struct _OVL_DESC {
  HANDLE Handle;   /* 底层 NTFS 句柄 */
  PWSTR RelName;   /* 卷内相对名, 如 L"\\dir\\file", 根为 L"" */
  PWSTR UpperPath; /* upper 层全路径(即使尚不存在) */
  PWSTR LowerPath; /* 来源 lower 层全路径, 可空 */
  PVOID DirBuffer; /* ReadDirectory 缓存 */
  BOOLEAN IsDirectory;
  BOOLEAN UpperExists;
  BOOLEAN LowerExists;
} OVL_DESC;

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static NTSTATUS W32Err(DWORD e) { return FspNtStatusFromWin32(e); }

static PWSTR StrDup(PCWSTR s) {
  SIZE_T n = (wcslen(s) + 1) * sizeof(WCHAR);
  PWSTR p = malloc(n);
  if (p)
    memcpy(p, s, n);
  return p;
}

static PWSTR StrCat2(PCWSTR a, PCWSTR b) {
  SIZE_T la = wcslen(a), lb = wcslen(b);
  PWSTR p = malloc((la + lb + 1) * sizeof(WCHAR));
  if (p) {
    memcpy(p, a, la * 2);
    memcpy(p + la, b, (lb + 1) * 2);
  }
  return p;
}

static PWSTR StrCat3(PCWSTR a, PCWSTR b, PCWSTR c) {
  SIZE_T la = wcslen(a), lb = wcslen(b), lc = wcslen(c);
  PWSTR p = malloc((la + lb + lc + 1) * sizeof(WCHAR));
  if (p) {
    memcpy(p, a, la * 2);
    memcpy(p + la, b, lb * 2);
    memcpy(p + la + lb, c, (lc + 1) * 2);
  }
  return p;
}

/* 规范化目录路径: 全路径、大写盘符、去尾部反斜杠 */
static BOOLEAN NormalizeRootDir(PCWSTR In, PWSTR Out, ULONG OutChars) {
  WCHAR Tmp[MAX_PATH];
  DWORD n = GetFullPathNameW(In, MAX_PATH, Tmp, 0);
  if (n == 0 || n >= MAX_PATH)
    return FALSE;
  while (n > 3 && (Tmp[n - 1] == L'\\' || Tmp[n - 1] == L'/'))
    Tmp[--n] = 0;
  if (n >= 2 && Tmp[1] == L':') {
    if (Tmp[0] >= L'a' && Tmp[0] <= L'z')
      Tmp[0] -= 32;
    for (PWSTR s = Tmp + 2; *s; s++)
      if (*s == L'/')
        *s = L'\\';
  }
  if (n + 1 > OutChars)
    return FALSE;
  memcpy(Out, Tmp, (n + 1) * sizeof(WCHAR));
  return TRUE;
}

static VOID FreeDesc(OVL_DESC *Desc) {
  if (Desc->Handle != INVALID_HANDLE_VALUE)
    CloseHandle(Desc->Handle);
  FspFileSystemDeleteDirectoryBuffer(&Desc->DirBuffer);
  free(Desc->RelName);
  free(Desc->UpperPath);
  free(Desc->LowerPath);
  free(Desc);
}

static OVL_DESC *AllocDesc(PCWSTR Rel, PCWSTR Upper,
                           PWSTR Lower /* 转移所有权 */, HANDLE Handle,
                           BOOLEAN IsDir, BOOLEAN UpExists, BOOLEAN LoExists) {
  OVL_DESC *d = calloc(1, sizeof *d);
  if (!d) {
    if (Lower)
      free(Lower);
    return 0;
  }
  d->Handle = Handle;
  d->RelName = StrDup(Rel);
  d->UpperPath = StrDup(Upper);
  d->LowerPath = Lower;
  d->IsDirectory = IsDir;
  d->UpperExists = UpExists;
  d->LowerExists = LoExists;
  return d;
}

static VOID GetFileInfoFromHandle(HANDLE h, FSP_FSCTL_FILE_INFO *Fi) {
  BY_HANDLE_FILE_INFORMATION Bh;
  memset(Fi, 0, sizeof *Fi);
  if (!GetFileInformationByHandle(h, &Bh))
    return;
  UINT64 Size = ((UINT64)Bh.nFileSizeHigh << 32) | Bh.nFileSizeLow;
  Fi->FileAttributes = Bh.dwFileAttributes;
  Fi->FileSize = Size;
  Fi->AllocationSize = (Size + g_AllocUnit - 1) & ~(g_AllocUnit - 1);
  Fi->CreationTime = *(UINT64 *)&Bh.ftCreationTime;
  Fi->LastAccessTime = *(UINT64 *)&Bh.ftLastAccessTime;
  Fi->LastWriteTime = *(UINT64 *)&Bh.ftLastWriteTime;
  Fi->ChangeTime = *(UINT64 *)&Bh.ftLastWriteTime;
  Fi->IndexNumber = ((UINT64)Bh.nFileIndexHigh << 32) | Bh.nFileIndexLow;
}

static VOID GetFileInfoFromFindData(const WIN32_FIND_DATAW *Fd,
                                    FSP_FSCTL_FILE_INFO *Fi) {
  UINT64 Size = ((UINT64)Fd->nFileSizeHigh << 32) | Fd->nFileSizeLow;
  memset(Fi, 0, sizeof *Fi);
  Fi->FileAttributes = Fd->dwFileAttributes;
  Fi->FileSize = Size;
  Fi->AllocationSize = (Size + g_AllocUnit - 1) & ~(g_AllocUnit - 1);
  Fi->CreationTime = *(UINT64 *)&Fd->ftCreationTime;
  Fi->LastAccessTime = *(UINT64 *)&Fd->ftLastAccessTime;
  Fi->LastWriteTime = *(UINT64 *)&Fd->ftLastWriteTime;
  Fi->ChangeTime = *(UINT64 *)&Fd->ftLastWriteTime;
}

/* 打开底层 NTFS 文件; 只读打开仅授予 GENERIC_READ */
static HANDLE OpenUnderlying(PCWSTR Path, BOOLEAN IsDir, BOOLEAN Writable,
                             UINT32 CreateOptions) {
  DWORD Access = GENERIC_READ | READ_CONTROL | SYNCHRONIZE;
  DWORD Flags = 0;
  if (Writable)
    Access |= GENERIC_WRITE;
  if (IsDir)
    Flags |= FILE_FLAG_BACKUP_SEMANTICS;
  else if (CreateOptions & FILE_WRITE_THROUGH)
    Flags |= FILE_FLAG_WRITE_THROUGH;
  return CreateFileW(Path, Access,
                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0,
                     OPEN_EXISTING, Flags, 0);
}

/* 把打开句柄的真实大小写名字回报给 FSD (文件名规范化) */
static VOID SetNormalizedName(HANDLE Handle, PCWSTR RootUsed,
                              FSP_FSCTL_FILE_INFO *FileInfo) {
  FSP_FSCTL_OPEN_FILE_INFO *Ofi = FspFileSystemGetOpenFileInfo(FileInfo);
  WCHAR Buf[1024];
  if (!Ofi->NormalizedName)
    return;
  DWORD n = GetFinalPathNameByHandleW(Handle, Buf, 1024, VOLUME_NAME_NONE);
  if (n == 0 || n >= 1024)
    return;
  PCWSTR RootLocal = RootUsed; /* "C:\\xxx" -> "\\xxx" */
  if (RootLocal[0] && RootLocal[1] == L':')
    RootLocal += 2;
  SIZE_T rl = wcslen(RootLocal);
  if (_wcsnicmp(Buf, RootLocal, rl) != 0)
    return;
  PCWSTR Rel = Buf + rl;
  SIZE_T Bytes = wcslen(Rel) * sizeof(WCHAR);
  if (Bytes <= Ofi->NormalizedNameSize) {
    memcpy(Ofi->NormalizedName, Rel, Bytes);
    Ofi->NormalizedNameSize = (UINT16)Bytes;
  }
}

/* ------------------------------------------------------------------ */
/* 合并视图查找 / whiteout                                             */
/* ------------------------------------------------------------------ */

static BOOLEAN FindLowerPath(PCWSTR Rel, PWSTR *PPath) {
  for (ULONG i = 0; i < g_LowerCount; i++) {
    PWSTR p = StrCat2(g_LowerRoots[i], Rel);
    if (!p)
      continue;
    if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) {
      *PPath = p;
      return TRUE;
    }
    free(p);
  }
  return FALSE;
}

static PWSTR WhiteoutPathOf(PCWSTR Rel) {
  PCWSTR Slash = wcsrchr(Rel, L'\\');
  if (!Slash)
    return 0;
  SIZE_T ParentLen = Slash - Rel;
  PCWSTR Leaf = Slash + 1;
  SIZE_T Total = wcslen(g_UpperRoot) + ParentLen + 5 + wcslen(Leaf) + 1;
  PWSTR p = malloc(Total * sizeof(WCHAR));
  if (!p)
    return 0;
  wcscpy_s(p, Total, g_UpperRoot);
  wcsncat_s(p, Total, Rel, ParentLen);
  wcscat_s(p, Total, L"\\" WHITEOUT_PREFIX);
  wcscat_s(p, Total, Leaf);
  return p;
}

static BOOLEAN WhiteoutExistsAt(PCWSTR Rel) {
  BOOLEAN b = FALSE;
  PWSTR wp = WhiteoutPathOf(Rel);
  if (wp) {
    b = GetFileAttributesW(wp) != INVALID_FILE_ATTRIBUTES;
    free(wp);
  }
  return b;
}

static VOID MakeWhiteout(PCWSTR Rel); /* 前向声明 */
static NTSTATUS EnsureUpperParents(PCWSTR Rel);

static BOOLEAN MergedLookup(PCWSTR Rel, PUINT32 PAttr) {
  PWSTR up = StrCat2(g_UpperRoot, Rel);
  if (up) {
    DWORD a = GetFileAttributesW(up);
    free(up);
    if (a != INVALID_FILE_ATTRIBUTES) {
      *PAttr = a;
      return TRUE;
    }
  }
  if (WhiteoutExistsAt(Rel))
    return FALSE;
  PWSTR lp;
  if (FindLowerPath(Rel, &lp)) {
    *PAttr = GetFileAttributesW(lp);
    free(lp);
    return TRUE;
  }
  return FALSE;
}

/* ------------------------------------------------------------------ */
/* upper 父目录惰性 copy-up                                            */
/* ------------------------------------------------------------------ */

static VOID CopyFileMeta(PCWSTR Src, PCWSTR Dst) {
  DWORD A = GetFileAttributesW(Src);
  if (A != INVALID_FILE_ATTRIBUTES)
    SetFileAttributesW(Dst, A & ~FILE_ATTRIBUTE_READONLY);
  HANDLE Sh =
      CreateFileW(Src, FILE_READ_ATTRIBUTES,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0,
                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0);
  HANDLE Dh =
      CreateFileW(Dst, FILE_WRITE_ATTRIBUTES,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0,
                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0);
  if (Sh != INVALID_HANDLE_VALUE && Dh != INVALID_HANDLE_VALUE) {
    FILETIME C, A2, W2;
    if (GetFileTime(Sh, &C, &A2, &W2))
      SetFileTime(Dh, &C, &A2, &W2);
  }
  if (Sh != INVALID_HANDLE_VALUE)
    CloseHandle(Sh);
  if (Dh != INVALID_HANDLE_VALUE)
    CloseHandle(Dh);
}

static NTSTATUS EnsureUpperDir(PCWSTR Rel) {
  PWSTR up = StrCat2(g_UpperRoot, Rel);
  if (!up)
    return STATUS_NO_MEMORY;
  if (GetFileAttributesW(up) != INVALID_FILE_ATTRIBUTES) {
    free(up);
    return STATUS_SUCCESS;
  }
  PWSTR lp;
  if (!FindLowerPath(Rel, &lp)) {
    free(up);
    return STATUS_OBJECT_PATH_NOT_FOUND;
  }
  if (!CreateDirectoryW(up, 0) && GetLastError() != ERROR_ALREADY_EXISTS) {
    NTSTATUS R = W32Err(GetLastError());
    free(up);
    free(lp);
    return R;
  }
  CopyFileMeta(lp, up);
  free(up);
  free(lp);
  return STATUS_SUCCESS;
}

static NTSTATUS EnsureUpperParents(PCWSTR Rel) {
  PWSTR p = StrDup(Rel);
  if (!p)
    return STATUS_NO_MEMORY;
  NTSTATUS R = STATUS_SUCCESS;
  for (PWSTR s = p + 1; *s; s++) {
    if (*s == L'\\') {
      *s = 0;
      R = EnsureUpperDir(p);
      *s = L'\\';
      if (!NT_SUCCESS(R))
        break;
    }
  }
  free(p);
  return R;
}

/* ------------------------------------------------------------------ */
/* copy-up 引擎 (同路径并发去重)                                       */
/* ------------------------------------------------------------------ */

static NTSTATUS CopyFileRaw(PCWSTR Src, PCWSTR Dst) {
  NTSTATUS R = STATUS_SUCCESS;
  HANDLE Sh = CreateFileW(
      Src, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      0, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, 0);
  if (Sh == INVALID_HANDLE_VALUE)
    return W32Err(GetLastError());
  HANDLE Dh =
      CreateFileW(Dst, GENERIC_WRITE | FILE_WRITE_ATTRIBUTES,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0,
                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, 0);
  if (Dh == INVALID_HANDLE_VALUE) {
    DWORD e = GetLastError();
    CloseHandle(Sh);
    if (e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS)
      return STATUS_SUCCESS; /* 竞争: 别人已复制 */
    return W32Err(e);
  }
  PVOID Buf = malloc(COPY_BUF_SIZE);
  if (!Buf)
    R = STATUS_NO_MEMORY;
  else
    for (;;) {
      DWORD Got;
      if (!ReadFile(Sh, Buf, COPY_BUF_SIZE, &Got, 0)) {
        R = W32Err(GetLastError());
        break;
      }
      if (Got == 0)
        break;
      DWORD Wr;
      ULONG_PTR Off = 0;
      while (Got) {
        if (!WriteFile(Dh, (PUINT8)Buf + Off, Got, &Wr, 0)) {
          R = W32Err(GetLastError());
          break;
        }
        Got -= Wr;
        Off += Wr;
      }
      if (!NT_SUCCESS(R))
        break;
    }
  if (NT_SUCCESS(R)) {
    FILETIME C, A2, W2;
    if (GetFileTime(Sh, &C, &A2, &W2))
      SetFileTime(Dh, &C, &A2, &W2);
  }
  free(Buf);
  CloseHandle(Dh);
  CloseHandle(Sh);
  if (NT_SUCCESS(R)) {
    DWORD A = GetFileAttributesW(Src);
    if (A != INVALID_FILE_ATTRIBUTES)
      SetFileAttributesW(Dst, A & ~FILE_ATTRIBUTE_READONLY);
  }
  return R;
}

static NTSTATUS CopyUpOne(PCWSTR Lower, PCWSTR Upper) {
  DWORD A = GetFileAttributesW(Lower);
  if (A == INVALID_FILE_ATTRIBUTES)
    return W32Err(GetLastError());
  if (A & FILE_ATTRIBUTE_DIRECTORY) {
    if (!CreateDirectoryW(Upper, 0) && GetLastError() != ERROR_ALREADY_EXISTS)
      return W32Err(GetLastError());
    CopyFileMeta(Lower, Upper);
    return STATUS_SUCCESS;
  }
  return CopyFileRaw(Lower, Upper);
}

static VOID CopyUpRelease(OVL_COPYUP *Cu) {
  EnterCriticalSection(&g_CopyUpCs);
  if (--Cu->Refs == 0) {
    LeaveCriticalSection(&g_CopyUpCs);
    CloseHandle(Cu->Event);
    free(Cu->Path);
    free(Cu);
    return;
  }
  LeaveCriticalSection(&g_CopyUpCs);
}

static NTSTATUS CopyUpPath(PCWSTR Lower, PCWSTR Upper) {
  OVL_COPYUP *Cu, **Slot;
  BOOLEAN Mine = FALSE;
  EnterCriticalSection(&g_CopyUpCs);
  for (Cu = g_CopyUps; Cu; Cu = Cu->Next)
    if (_wcsicmp(Cu->Path, Upper) == 0) {
      Cu->Refs++;
      break;
    }
  if (!Cu) {
    Cu = calloc(1, sizeof *Cu);
    if (!Cu) {
      LeaveCriticalSection(&g_CopyUpCs);
      return STATUS_NO_MEMORY;
    }
    Cu->Path = StrDup(Upper);
    Cu->Event = CreateEventW(0, TRUE, FALSE, 0);
    Cu->Refs = 1;
    Cu->Next = g_CopyUps;
    g_CopyUps = Cu;
    Mine = TRUE;
  }
  LeaveCriticalSection(&g_CopyUpCs);

  NTSTATUS R;
  if (Mine) {
    R = CopyUpOne(Lower, Upper);
    EnterCriticalSection(&g_CopyUpCs);
    Cu->Status = R;
    for (Slot = &g_CopyUps; *Slot != Cu; Slot = &(*Slot)->Next)
      ;
    *Slot = Cu->Next;
    LeaveCriticalSection(&g_CopyUpCs);
    SetEvent(Cu->Event);
  } else {
    WaitForSingleObject(Cu->Event, INFINITE);
    R = Cu->Status;
  }
  CopyUpRelease(Cu);
  return R;
}

/* 描述符升级到可写: 必要时 copy-up 并替换句柄 */
static NTSTATUS EnsureUpperWritable(OVL_DESC *Desc) {
  if (Desc->UpperExists)
    return STATUS_SUCCESS;
  if (!Desc->LowerPath)
    return STATUS_OBJECT_NAME_NOT_FOUND;
  NTSTATUS R = EnsureUpperParents(Desc->RelName);
  if (!NT_SUCCESS(R))
    return R;
  R = CopyUpPath(Desc->LowerPath, Desc->UpperPath);
  if (!NT_SUCCESS(R))
    return R;
  HANDLE Nh = OpenUnderlying(Desc->UpperPath, Desc->IsDirectory, TRUE, 0);
  if (Nh == INVALID_HANDLE_VALUE)
    return W32Err(GetLastError());
  CloseHandle(Desc->Handle);
  Desc->Handle = Nh;
  Desc->UpperExists = TRUE;
  return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* whiteout / 清理                                                     */
/* ------------------------------------------------------------------ */

static VOID MakeWhiteout(PCWSTR Rel) {
  PWSTR wp = WhiteoutPathOf(Rel);
  if (!wp)
    return;
  EnsureUpperParents(Rel);
  HANDLE h = CreateFileW(wp, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                         0, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, 0);
  if (h != INVALID_HANDLE_VALUE)
    CloseHandle(h);
  free(wp);
}

static VOID RemoveWhiteout(PCWSTR Rel) {
  PWSTR wp = WhiteoutPathOf(Rel);
  if (!wp)
    return;
  DeleteFileW(wp);
  free(wp);
}

/* 递归删除 upper 目录树中的所有 whiteout (物理删除 upper 目录前需要) */
static VOID PurgeWhiteoutsRecursive(PWSTR DirPath) {
  PWSTR Pat = StrCat2(DirPath, L"\\*");
  if (!Pat)
    return;
  WIN32_FIND_DATAW Fd;
  HANDLE F = FindFirstFileExW(Pat, FindExInfoBasic, &Fd, FindExSearchNameMatch,
                              0, FIND_FIRST_EX_LARGE_FETCH);
  free(Pat);
  if (F == INVALID_HANDLE_VALUE)
    return;
  do {
    if (wcscmp(Fd.cFileName, L".") == 0 || wcscmp(Fd.cFileName, L"..") == 0)
      continue;
    PWSTR Full = StrCat3(DirPath, L"\\", Fd.cFileName);
    if (!Full)
      continue;
    if (Fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      PurgeWhiteoutsRecursive(Full);
    else if (wcsncmp(Fd.cFileName, WHITEOUT_PREFIX, WHITEOUT_PREFIX_LEN) == 0) {
      SetFileAttributesW(Full, FILE_ATTRIBUTE_NORMAL);
      DeleteFileW(Full);
    }
    free(Full);
  } while (FindNextFileW(F, &Fd));
  FindClose(F);
}

/* ------------------------------------------------------------------ */
/* 名字哈希集合 (目录合并去重)                                         */
/* ------------------------------------------------------------------ */

typedef struct _NAMESET {
  PWSTR *Items;
  ULONG Count, Capacity;
} NAMESET;

static ULONG NameHash(PCWSTR s) {
  ULONG h = 2166136261u;
  for (; *s; s++) {
    WCHAR c = *s;
    if (c >= L'A' && c <= L'Z')
      c += 32;
    h = (h ^ (USHORT)c) * 16777619u;
  }
  return h;
}

static BOOLEAN NameSetInit(NAMESET *Set, ULONG Cap) {
  Set->Count = 0;
  Set->Capacity = Cap;
  Set->Items = calloc(Cap, sizeof(PWSTR));
  return Set->Items != 0;
}

static VOID NameSetFree(NAMESET *Set) {
  for (ULONG i = 0; i < Set->Capacity; i++)
    free(Set->Items[i]);
  free(Set->Items);
}

static BOOLEAN NameSetInsert(NAMESET *Set, PCWSTR Name) /* TRUE=已存在 */
{
  if ((Set->Count + 1) * 10 >= Set->Capacity * 7) {
    ULONG NewCap = Set->Capacity * 2;
    PWSTR *New = calloc(NewCap, sizeof(PWSTR));
    if (!New)
      return TRUE;
    for (ULONG i = 0; i < Set->Capacity; i++) {
      PWSTR e = Set->Items[i];
      if (!e)
        continue;
      ULONG j = NameHash(e) & (NewCap - 1);
      while (New[j])
        j = (j + 1) & (NewCap - 1);
      New[j] = e;
    }
    free(Set->Items);
    Set->Items = New;
    Set->Capacity = NewCap;
  }
  ULONG i = NameHash(Name) & (Set->Capacity - 1);
  for (;;) {
    PWSTR e = Set->Items[i];
    if (!e) {
      Set->Items[i] = StrDup(Name);
      Set->Count++;
      return FALSE;
    }
    if (_wcsicmp(e, Name) == 0)
      return TRUE;
    i = (i + 1) & (Set->Capacity - 1);
  }
}

/* ------------------------------------------------------------------ */
/* 文件系统操作实现                                                    */
/* ------------------------------------------------------------------ */

static NTSTATUS OvlGetVolumeInfo(FSP_FILE_SYSTEM *Fs,
                                 FSP_FSCTL_VOLUME_INFO *VolumeInfo) {
  (void)Fs;
  ULARGE_INTEGER FreeAvail, Total, TotalFree;
  if (!GetDiskFreeSpaceExW(g_UpperRoot, &FreeAvail, &Total, &TotalFree))
    return W32Err(GetLastError());
  memset(VolumeInfo, 0, sizeof *VolumeInfo);
  VolumeInfo->TotalSize = Total.QuadPart;
  VolumeInfo->FreeSize = TotalFree.QuadPart;
  VolumeInfo->VolumeLabelLength = (UINT16)(wcslen(g_Label) * sizeof(WCHAR));
  memcpy(VolumeInfo->VolumeLabel, g_Label, VolumeInfo->VolumeLabelLength);
  return STATUS_SUCCESS;
}

static NTSTATUS OvlSetVolumeLabel(FSP_FILE_SYSTEM *Fs, PWSTR VolumeLabel,
                                  FSP_FSCTL_VOLUME_INFO *VolumeInfo) {
  (void)Fs;
  wcsncpy_s(g_Label, 32, VolumeLabel, _TRUNCATE);
  return OvlGetVolumeInfo(0, VolumeInfo);
}

/* 不做安全检查: 只报属性 + NULL DACL */
static NTSTATUS OvlGetSecurityByName(FSP_FILE_SYSTEM *Fs, PWSTR FileName,
                                     PUINT32 PFileAttributes,
                                     PSECURITY_DESCRIPTOR SecurityDescriptor,
                                     SIZE_T *PSecurityDescriptorSize) {
  (void)Fs;
  (void)SecurityDescriptor;
  UINT32 Attr;
  if (!MergedLookup(FileName, &Attr))
    return STATUS_OBJECT_NAME_NOT_FOUND;
  if (PFileAttributes)
    *PFileAttributes = Attr;
  if (PSecurityDescriptorSize)
    *PSecurityDescriptorSize = 0; /* 不返回任何 SD */
  return STATUS_SUCCESS;
}

static NTSTATUS OvlCreate(FSP_FILE_SYSTEM *Fs, PWSTR FileName,
                          UINT32 CreateOptions, UINT32 GrantedAccess,
                          UINT32 FileAttributes,
                          PSECURITY_DESCRIPTOR SecurityDescriptor,
                          UINT64 AllocationSize, PVOID *PFileContext,
                          FSP_FSCTL_FILE_INFO *FileInfo) {
  (void)Fs;
  (void)GrantedAccess;
  (void)SecurityDescriptor;
  BOOLEAN IsDir = (CreateOptions & FILE_DIRECTORY_FILE) != 0;
  NTSTATUS R = EnsureUpperParents(FileName);
  if (!NT_SUCCESS(R))
    return R;

  PWSTR up = StrCat2(g_UpperRoot, FileName);
  if (!up)
    return STATUS_NO_MEMORY;

  HANDLE h;
  if (IsDir) {
    if (!CreateDirectoryW(up, 0)) {
      R = W32Err(GetLastError());
      free(up);
      return R;
    }
    if (FileAttributes)
      SetFileAttributesW(up, FileAttributes);
    h = OpenUnderlying(up, TRUE, TRUE, CreateOptions);
  } else {
    h = CreateFileW(up, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0,
                    CREATE_NEW,
                    FileAttributes ? FileAttributes : FILE_ATTRIBUTE_NORMAL, 0);
    if (h != INVALID_HANDLE_VALUE && AllocationSize) {
      FILE_ALLOCATION_INFO AllocInfo;
      AllocInfo.AllocationSize.QuadPart = (LONGLONG)AllocationSize;
      SetFileInformationByHandle(h, FileAllocationInfo, &AllocInfo,
                                 sizeof(AllocInfo));
    }
  }
  if (h == INVALID_HANDLE_VALUE) {
    R = W32Err(GetLastError());
    free(up);
    return R;
  }

  RemoveWhiteout(FileName);

  PWSTR lower = 0;
  BOOLEAN LowerExists = FindLowerPath(FileName, &lower);
  OVL_DESC *d = AllocDesc(FileName, up, lower, h, IsDir, TRUE, LowerExists);
  free(up);
  if (!d) {
    CloseHandle(h);
    return STATUS_NO_MEMORY;
  }
  *PFileContext = d;
  GetFileInfoFromHandle(h, FileInfo);
  SetNormalizedName(h, d->UpperPath, FileInfo);
  return STATUS_SUCCESS;
}

static NTSTATUS OvlOpen(FSP_FILE_SYSTEM *Fs, PWSTR FileName,
                        UINT32 CreateOptions, UINT32 GrantedAccess,
                        PVOID *PFileContext, FSP_FSCTL_FILE_INFO *FileInfo) {
  (void)Fs;
  BOOLEAN Writable =
      (GrantedAccess & (FILE_WRITE_DATA | FILE_APPEND_DATA |
                        FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA)) != 0;
  PWSTR up = StrCat2(g_UpperRoot, FileName);
  if (!up)
    return STATUS_NO_MEMORY;

  DWORD upAttr = GetFileAttributesW(up);
  BOOLEAN UpperExists = upAttr != INVALID_FILE_ATTRIBUTES;
  PWSTR lower = 0;
  BOOLEAN LowerExists = FALSE;
  HANDLE h = INVALID_HANDLE_VALUE;
  BOOLEAN Copied = FALSE;
  NTSTATUS R;

  if (UpperExists) {
    h = OpenUnderlying(up, (upAttr & FILE_ATTRIBUTE_DIRECTORY) != 0, Writable,
                       CreateOptions);
    if (h == INVALID_HANDLE_VALUE) {
      R = W32Err(GetLastError());
      free(up);
      return R;
    }
    LowerExists = FindLowerPath(FileName, &lower);
  } else {
    if (WhiteoutExistsAt(FileName)) {
      free(up);
      return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    if (!FindLowerPath(FileName, &lower)) {
      free(up);
      return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    LowerExists = TRUE;
    BOOLEAN IsDir = (GetFileAttributesW(lower) & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (Writable) {
      /* 写打开 => 立即 copy-up */
      R = EnsureUpperParents(FileName);
      if (NT_SUCCESS(R))
        R = CopyUpPath(lower, up);
      if (!NT_SUCCESS(R)) {
        free(up);
        free(lower);
        return R;
      }
      h = OpenUnderlying(up, IsDir, TRUE, CreateOptions);
      if (h == INVALID_HANDLE_VALUE) {
        R = W32Err(GetLastError());
        free(up);
        free(lower);
        return R;
      }
      RemoveWhiteout(FileName);
      Copied = TRUE;
    } else {
      /* 只读打开 => 直接透传 lower 层句柄, 零拷贝 */
      h = OpenUnderlying(lower, IsDir, FALSE, CreateOptions);
      if (h == INVALID_HANDLE_VALUE) {
        R = W32Err(GetLastError());
        free(up);
        free(lower);
        return R;
      }
    }
  }

  BOOLEAN IsDir2 = FALSE;
  BY_HANDLE_FILE_INFORMATION Bh;
  if (GetFileInformationByHandle(h, &Bh))
    IsDir2 = (Bh.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

  OVL_DESC *d = AllocDesc(FileName, up, lower, h, IsDir2, UpperExists || Copied,
                          LowerExists);
  free(up);
  if (!d) {
    CloseHandle(h);
    return STATUS_NO_MEMORY;
  }
  *PFileContext = d;
  GetFileInfoFromHandle(h, FileInfo);
  SetNormalizedName(h, d->UpperExists ? d->UpperPath : d->LowerPath, FileInfo);
  return STATUS_SUCCESS;
}

static NTSTATUS OvlOverwrite(FSP_FILE_SYSTEM *Fs, PVOID FileContext,
                             UINT32 FileAttributes,
                             BOOLEAN ReplaceFileAttributes,
                             UINT64 AllocationSize,
                             FSP_FSCTL_FILE_INFO *FileInfo) {
  (void)Fs;
  OVL_DESC *Desc = FileContext;
  NTSTATUS R;

  if (!Desc->UpperExists) {
    /* 覆盖 lower-only 文件: 无需复制旧数据, 直接在 upper 建空文件 */
    R = EnsureUpperParents(Desc->RelName);
    if (!NT_SUCCESS(R))
      return R;
    HANDLE h =
        CreateFileW(Desc->UpperPath, GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0,
                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE)
      return W32Err(GetLastError());
    CloseHandle(Desc->Handle);
    Desc->Handle = h;
    Desc->UpperExists = TRUE;
    RemoveWhiteout(Desc->RelName);
  } else {
    /* [FIX] upper 文件已存在: 覆盖语义必须先把内容清空 */
    LARGE_INTEGER li;
    li.QuadPart = 0;
    SetFilePointerEx(Desc->Handle, li, 0, FILE_BEGIN);
    if (!SetEndOfFile(Desc->Handle))
      return W32Err(GetLastError());
  }

  if (AllocationSize) {
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)AllocationSize;
    SetFilePointerEx(Desc->Handle, li, 0, FILE_BEGIN);
    SetEndOfFile(Desc->Handle);
    li.QuadPart = 0;
    SetFilePointerEx(Desc->Handle, li, 0, FILE_BEGIN);
  }

  if (FileAttributes) {
    DWORD cur = GetFileAttributesW(Desc->UpperPath);
    if (cur != INVALID_FILE_ATTRIBUTES)
      SetFileAttributesW(Desc->UpperPath, ReplaceFileAttributes
                                              ? FileAttributes
                                              : (cur | FileAttributes));
  }

  PWSTR tmp = 0;
  Desc->LowerExists = FindLowerPath(Desc->RelName, &tmp);
  if (tmp)
    free(tmp);
  GetFileInfoFromHandle(Desc->Handle, FileInfo);
  return STATUS_SUCCESS;
}

static VOID OvlCleanup(FSP_FILE_SYSTEM *Fs, PVOID FileContext, PWSTR FileName,
                       ULONG Flags) {
  (void)Fs;
  (void)FileName;
  OVL_DESC *Desc = FileContext;
  if (!(Flags & FspCleanupDelete))
    return; /* 时间/归档位等由底层 NTFS 自行维护 */

  if (Desc->UpperExists) {
    if (Desc->LowerExists)
      MakeWhiteout(Desc->RelName);
    if (Desc->IsDirectory) {
      PurgeWhiteoutsRecursive(Desc->UpperPath);
      RemoveDirectoryW(Desc->UpperPath);
    } else
      DeleteFileW(Desc->UpperPath);
    if (!Desc->LowerExists)
      RemoveWhiteout(Desc->RelName);
  } else if (Desc->LowerExists) {
    /* 只删 lower 层条目: 只建 whiteout, 不复制数据 */
    MakeWhiteout(Desc->RelName);
  }
}

static VOID OvlClose(FSP_FILE_SYSTEM *Fs, PVOID FileContext) {
  (void)Fs;
  FreeDesc(FileContext);
}

static NTSTATUS OvlRead(FSP_FILE_SYSTEM *Fs, PVOID FileContext, PVOID Buffer,
                        UINT64 Offset, ULONG Length, PULONG PBytesTransferred) {
  (void)Fs;
  OVL_DESC *Desc = FileContext;
  OVERLAPPED Ov;
  memset(&Ov, 0, sizeof Ov);
  Ov.Offset = (DWORD)Offset;
  Ov.OffsetHigh = (DWORD)(Offset >> 32);
  DWORD n;
  if (!ReadFile(Desc->Handle, Buffer, Length, &n, &Ov)) {
    DWORD e = GetLastError();
    if (e == ERROR_HANDLE_EOF)
      return STATUS_END_OF_FILE;
    return W32Err(e);
  }
  *PBytesTransferred = n;
  return STATUS_SUCCESS;
}

static NTSTATUS OvlWrite(FSP_FILE_SYSTEM *Fs, PVOID FileContext, PVOID Buffer,
                         UINT64 Offset, ULONG Length, BOOLEAN WriteToEndOfFile,
                         BOOLEAN ConstrainedIo, PULONG PBytesTransferred,
                         FSP_FSCTL_FILE_INFO *FileInfo) {
  (void)Fs;
  OVL_DESC *Desc = FileContext;
  *PBytesTransferred = 0;
  if (Length == 0) {
    GetFileInfoFromHandle(Desc->Handle, FileInfo);
    return STATUS_SUCCESS;
  }

  if (WriteToEndOfFile) {
    LARGE_INTEGER fs;
    if (!GetFileSizeEx(Desc->Handle, &fs))
      return W32Err(GetLastError());
    Offset = (UINT64)fs.QuadPart;
  } else if (ConstrainedIo) {
    LARGE_INTEGER fs;
    if (!GetFileSizeEx(Desc->Handle, &fs))
      return W32Err(GetLastError());
    if (Offset >= (UINT64)fs.QuadPart) {
      GetFileInfoFromHandle(Desc->Handle, FileInfo);
      return STATUS_SUCCESS;
    }
    if (Offset + Length > (UINT64)fs.QuadPart)
      Length = (ULONG)((UINT64)fs.QuadPart - Offset);
  }

  OVERLAPPED Ov;
  memset(&Ov, 0, sizeof Ov);
  Ov.Offset = (DWORD)Offset;
  Ov.OffsetHigh = (DWORD)(Offset >> 32);
  DWORD n;
  if (!WriteFile(Desc->Handle, Buffer, Length, &n, &Ov))
    return W32Err(GetLastError());
  *PBytesTransferred = n;
  GetFileInfoFromHandle(Desc->Handle, FileInfo);
  return STATUS_SUCCESS;
}

static NTSTATUS OvlFlush(FSP_FILE_SYSTEM *Fs, PVOID FileContext,
                         FSP_FSCTL_FILE_INFO *FileInfo) {
  (void)Fs;
  if (!FileContext)
    return STATUS_SUCCESS; /* 卷刷新: FSD 已刷缓存 */
  OVL_DESC *Desc = FileContext;
  if (!FlushFileBuffers(Desc->Handle))
    return W32Err(GetLastError());
  GetFileInfoFromHandle(Desc->Handle, FileInfo);
  return STATUS_SUCCESS;
}

static NTSTATUS OvlGetFileInfo(FSP_FILE_SYSTEM *Fs, PVOID FileContext,
                               FSP_FSCTL_FILE_INFO *FileInfo) {
  (void)Fs;
  GetFileInfoFromHandle(((OVL_DESC *)FileContext)->Handle, FileInfo);
  return STATUS_SUCCESS;
}

static NTSTATUS OvlSetBasicInfo(FSP_FILE_SYSTEM *Fs, PVOID FileContext,
                                UINT32 FileAttributes, UINT64 CreationTime,
                                UINT64 LastAccessTime, UINT64 LastWriteTime,
                                UINT64 ChangeTime,
                                FSP_FSCTL_FILE_INFO *FileInfo) {
  (void)Fs;
  (void)ChangeTime;
  OVL_DESC *Desc = FileContext;
  NTSTATUS R = EnsureUpperWritable(Desc);
  if (!NT_SUCCESS(R))
    return R;

  if (FileAttributes != INVALID_FILE_ATTRIBUTES &&
      !SetFileAttributesW(Desc->UpperPath, FileAttributes))
    return W32Err(GetLastError());

  if (CreationTime || LastAccessTime || LastWriteTime) {
    FILETIME C, A, W;
    if (CreationTime)
      *(UINT64 *)&C = CreationTime;
    if (LastAccessTime)
      *(UINT64 *)&A = LastAccessTime;
    if (LastWriteTime)
      *(UINT64 *)&W = LastWriteTime;
    SetFileTime(Desc->Handle, CreationTime ? &C : 0, LastAccessTime ? &A : 0,
                LastWriteTime ? &W : 0);
  }
  GetFileInfoFromHandle(Desc->Handle, FileInfo);
  return STATUS_SUCCESS;
}

static NTSTATUS OvlSetFileSize(FSP_FILE_SYSTEM *Fs, PVOID FileContext,
                               UINT64 NewSize, BOOLEAN SetAllocationSize,
                               FSP_FSCTL_FILE_INFO *FileInfo) {
  (void)Fs;
  (void)SetAllocationSize;
  OVL_DESC *Desc = FileContext;
  NTSTATUS R = EnsureUpperWritable(Desc);
  if (!NT_SUCCESS(R))
    return R;
  LARGE_INTEGER li;
  li.QuadPart = (LONGLONG)NewSize;
  if (!SetFilePointerEx(Desc->Handle, li, 0, FILE_BEGIN))
    return W32Err(GetLastError());
  if (!SetEndOfFile(Desc->Handle))
    return W32Err(GetLastError());
  GetFileInfoFromHandle(Desc->Handle, FileInfo);
  return STATUS_SUCCESS;
}

/* 递归复制目录树 (用于改名 lower-only 目录) */
static NTSTATUS OvlCopyTree(PCWSTR SrcDir, PCWSTR DstDir) {
  if (!CreateDirectoryW(DstDir, 0) && GetLastError() != ERROR_ALREADY_EXISTS)
    return W32Err(GetLastError());
  CopyFileMeta(SrcDir, DstDir);
  PWSTR Pat = StrCat2(SrcDir, L"\\*");
  if (!Pat)
    return STATUS_NO_MEMORY;
  WIN32_FIND_DATAW Fd;
  HANDLE F = FindFirstFileExW(Pat, FindExInfoBasic, &Fd, FindExSearchNameMatch,
                              0, FIND_FIRST_EX_LARGE_FETCH);
  free(Pat);
  if (F == INVALID_HANDLE_VALUE)
    return W32Err(GetLastError());
  NTSTATUS R = STATUS_SUCCESS;
  do {
    if (wcscmp(Fd.cFileName, L".") == 0 || wcscmp(Fd.cFileName, L"..") == 0)
      continue;
    PWSTR S = StrCat3(SrcDir, L"\\", Fd.cFileName);
    PWSTR D = StrCat3(DstDir, L"\\", Fd.cFileName);
    if (!S || !D) {
      free(S);
      free(D);
      R = STATUS_NO_MEMORY;
      break;
    }
    if (Fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      R = OvlCopyTree(S, D);
    else
      R = CopyFileRaw(S, D);
    free(S);
    free(D);
    if (!NT_SUCCESS(R))
      break;
  } while (FindNextFileW(F, &Fd));
  FindClose(F);
  return R;
}

static NTSTATUS OvlRename(FSP_FILE_SYSTEM *Fs, PVOID FileContext,
                          PWSTR FileName, PWSTR NewFileName,
                          BOOLEAN ReplaceIfExists) {
  (void)Fs;
  (void)FileName;
  OVL_DESC *Desc = FileContext;
  NTSTATUS R = EnsureUpperParents(NewFileName);
  if (!NT_SUCCESS(R))
    return R;

  PWSTR NewUpper = StrCat2(g_UpperRoot, NewFileName);
  if (!NewUpper)
    return STATUS_NO_MEMORY;

  if (Desc->IsDirectory &&
      GetFileAttributesW(NewUpper) != INVALID_FILE_ATTRIBUTES)
    PurgeWhiteoutsRecursive(
        NewUpper); /* 合并视图保证目标为空, 但 upper 里可能残留 whiteout */

  if (!Desc->UpperExists) {
    /* lower-only: 复制到 upper 新名字 + 旧名字打 whiteout (不搬动 lower) */
    R = EnsureUpperWritable(Desc); /* 目录则只建 upper 目录 */
    if (!NT_SUCCESS(R)) {
      free(NewUpper);
      return R;
    }
    if (Desc->IsDirectory) {
      R = OvlCopyTree(Desc->LowerPath, NewUpper);
      if (!NT_SUCCESS(R)) {
        free(NewUpper);
        return R;
      }
    } else {
      /* 文件: EnsureUpperWritable 已把数据复制到 UpperPath; 再改名 */
      if (!MoveFileExW(Desc->UpperPath, NewUpper,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        R = W32Err(GetLastError());
        DeleteFileW(Desc->UpperPath); /* 清理 copy-up 残留 */
        free(NewUpper);
        return R;
      }
    }
    /* [FIX] 必须隐藏 lower 层旧名, 否则改名后旧文件会"复活" */
    MakeWhiteout(Desc->RelName);
  } else {
    /* 目标只存在于 lower 层时, 用 whiteout 压住它 */
    UINT32 TAttr;
    if (ReplaceIfExists &&
        GetFileAttributesW(NewUpper) == INVALID_FILE_ATTRIBUTES &&
        !WhiteoutExistsAt(NewFileName) && MergedLookup(NewFileName, &TAttr) &&
        !(TAttr & FILE_ATTRIBUTE_DIRECTORY))
      MakeWhiteout(NewFileName);

    if (!MoveFileExW(Desc->UpperPath, NewUpper,
                     (ReplaceIfExists ? MOVEFILE_REPLACE_EXISTING : 0) |
                         MOVEFILE_WRITE_THROUGH)) {
      R = W32Err(GetLastError());
      free(NewUpper);
      return R;
    }
    /* 旧名字若在下层还有条目, 必须隐藏, 防止"复活" */
    if (Desc->LowerExists)
      MakeWhiteout(Desc->RelName);
    else
      RemoveWhiteout(Desc->RelName);
  }

  RemoveWhiteout(NewFileName);
  free(Desc->UpperPath);
  Desc->UpperPath = NewUpper;
  free(Desc->RelName);
  Desc->RelName = StrDup(NewFileName);
  PWSTR tmp = 0;
  Desc->LowerExists = FindLowerPath(NewFileName, &tmp);
  if (tmp)
    free(tmp);
  return STATUS_SUCCESS;
}

static NTSTATUS OvlEnumLayer(PWSTR DirPath, BOOLEAN IsUpper, NAMESET *Set,
                             PVOID *PDirBuf, PNTSTATUS PResult) {
  /* [FIX] upper 侧目录物理不存在 (lower-only 目录) 时按空目录处理 */
  DWORD A = GetFileAttributesW(DirPath);
  if (A == INVALID_FILE_ATTRIBUTES || !(A & FILE_ATTRIBUTE_DIRECTORY))
    return STATUS_SUCCESS;

  PWSTR Pat = StrCat2(DirPath, L"\\*");
  if (!Pat)
    return STATUS_NO_MEMORY;
  WIN32_FIND_DATAW Fd;
  HANDLE F = FindFirstFileExW(Pat, FindExInfoBasic, &Fd, FindExSearchNameMatch,
                              0, FIND_FIRST_EX_LARGE_FETCH);
  free(Pat);
  if (F == INVALID_HANDLE_VALUE)
    return W32Err(GetLastError());

  NTSTATUS R = STATUS_SUCCESS;
  do {
    if (IsUpper &&
        wcsncmp(Fd.cFileName, WHITEOUT_PREFIX, WHITEOUT_PREFIX_LEN) == 0 &&
        Fd.cFileName[WHITEOUT_PREFIX_LEN]) {
      /* whiteout: 登记被隐藏的名字, 不输出 */
      NameSetInsert(Set, Fd.cFileName + WHITEOUT_PREFIX_LEN);
      continue;
    }
    if (NameSetInsert(Set, Fd.cFileName))
      continue; /* upper 或前一个 lower 优先 */

    FSP_FSCTL_DIR_INFO Di;
    memset(&Di, 0, sizeof Di);
    SIZE_T Nb = (wcslen(Fd.cFileName) + 1) * sizeof(WCHAR);
    Di.Size = (UINT16)(FIELD_OFFSET(FSP_FSCTL_DIR_INFO, FileNameBuf) + Nb);
    GetFileInfoFromFindData(&Fd, &Di.FileInfo);
    memcpy(Di.FileNameBuf, Fd.cFileName, Nb);
    if (!FspFileSystemFillDirectoryBuffer(PDirBuf, &Di, PResult)) {
      R = *PResult;
      break;
    }
  } while (FindNextFileW(F, &Fd));
  FindClose(F);
  return R;
}

static NTSTATUS OvlReadDirectory(FSP_FILE_SYSTEM *Fs, PVOID FileContext,
                                 PWSTR Pattern, PWSTR Marker, PVOID Buffer,
                                 ULONG Length, PULONG PBytesTransferred) {
  (void)Fs;
  (void)Pattern; /* FSD 自行做通配符匹配 */
  OVL_DESC *Desc = FileContext;
  NTSTATUS Result = STATUS_SUCCESS;
  BOOLEAN Reset = (0 == Marker);

  if (!FspFileSystemAcquireDirectoryBuffer(&Desc->DirBuffer, Reset, &Result))
    return Result;

  if (Reset) {
    NAMESET Set;
    if (!NameSetInit(&Set, 256)) {
      FspFileSystemReleaseDirectoryBuffer(&Desc->DirBuffer);
      return STATUS_NO_MEMORY;
    }
    Result =
        OvlEnumLayer(Desc->UpperPath, TRUE, &Set, &Desc->DirBuffer, &Result);
    if (NT_SUCCESS(Result) && Desc->LowerPath)
      Result =
          OvlEnumLayer(Desc->LowerPath, FALSE, &Set, &Desc->DirBuffer, &Result);
    NameSetFree(&Set);
  }

  FspFileSystemReleaseDirectoryBuffer(&Desc->DirBuffer);
  FspFileSystemReadDirectoryBuffer(&Desc->DirBuffer, Marker, Buffer, Length,
                                   PBytesTransferred);
  return STATUS_SUCCESS;
}

static NTSTATUS OvlGetDirInfoByName(FSP_FILE_SYSTEM *Fs, PVOID FileContext,
                                    PWSTR FileName,
                                    FSP_FSCTL_DIR_INFO *DirInfo) {
  (void)Fs;
  OVL_DESC *Parent = FileContext;
  PWSTR ChildRel = StrCat3(Parent->RelName, L"\\", FileName);
  if (!ChildRel)
    return STATUS_NO_MEMORY;

  WIN32_FIND_DATAW Fd;
  HANDLE F = INVALID_HANDLE_VALUE;
  PWSTR up = StrCat2(g_UpperRoot, ChildRel);
  if (up) {
    F = FindFirstFileW(up, &Fd);
    free(up);
  }
  if (F == INVALID_HANDLE_VALUE) {
    if (WhiteoutExistsAt(ChildRel)) {
      free(ChildRel);
      return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    PWSTR lp;
    if (!FindLowerPath(ChildRel, &lp)) {
      free(ChildRel);
      return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    F = FindFirstFileW(lp, &Fd);
    free(lp);
    if (F == INVALID_HANDLE_VALUE) {
      free(ChildRel);
      return W32Err(GetLastError());
    }
  }
  free(ChildRel);

  SIZE_T Nb = (wcslen(FileName) + 1) * sizeof(WCHAR);
  memset(DirInfo, 0, sizeof *DirInfo);
  DirInfo->Size = (UINT16)(FIELD_OFFSET(FSP_FSCTL_DIR_INFO, FileNameBuf) + Nb);
  GetFileInfoFromFindData(&Fd, &DirInfo->FileInfo);
  memcpy(DirInfo->FileNameBuf, FileName, Nb);
  FindClose(F);
  return STATUS_SUCCESS;
}

static VOID OvlDispatcherStopped(FSP_FILE_SYSTEM *Fs, BOOLEAN Normally) {
  (void)Fs;
  (void)Normally;
  SetEvent(g_StopEvent);
}

/* ------------------------------------------------------------------ */
/* 主程序                                                              */
/* ------------------------------------------------------------------ */

static BOOL WINAPI CtrlHandler(DWORD Type) {
  switch (Type) {
  case CTRL_C_EVENT:
  case CTRL_BREAK_EVENT:
  case CTRL_CLOSE_EVENT:
  case CTRL_LOGOFF_EVENT:
  case CTRL_SHUTDOWN_EVENT:
    SetEvent(g_StopEvent);
    return TRUE;
  }
  return FALSE;
}

static VOID Usage(void) {
  fwprintf(stderr,
           L"ovlfs - 高性能 WinFsp OverlayFS\n"
           L"用法: ovlfs -u <upper目录> -l <lower目录>[;<lower2>...] [选项]\n"
           L"选项:\n"
           L"  -m <挂载点>   盘符(如 O: 或 O:)或目录; 缺省自动选择\n"
           L"  -i <毫秒>     内核元数据缓存超时, 0=关闭 (缺省 1000)\n"
           L"  -t <线程数>   分发线程数, 0=缺省\n"
           L"  -D <级别>     调试日志级别\n");
}

int wmain(int argc, wchar_t **argv) {
  _setmode(_fileno(stdout), _O_U16TEXT);
  _setmode(_fileno(stderr), _O_U16TEXT);

  PWSTR UpperArg = 0, LowerArg = 0, MountPoint = 0;
  ULONG InfoTimeout = 1000, Threads = 0, DebugLog = 0;

  for (int i = 1; i < argc; i++) {
    if (wcscmp(argv[i], L"-u") == 0 && i + 1 < argc)
      UpperArg = argv[++i];
    else if (wcscmp(argv[i], L"-l") == 0 && i + 1 < argc)
      LowerArg = argv[++i];
    else if (wcscmp(argv[i], L"-m") == 0 && i + 1 < argc)
      MountPoint = argv[++i];
    else if (wcscmp(argv[i], L"-i") == 0 && i + 1 < argc)
      InfoTimeout = _wtoi(argv[++i]);
    else if (wcscmp(argv[i], L"-t") == 0 && i + 1 < argc)
      Threads = _wtoi(argv[++i]);
    else if (wcscmp(argv[i], L"-D") == 0 && i + 1 < argc)
      DebugLog = _wtoi(argv[++i]);
    else {
      Usage();
      return 2;
    }
  }
  if (!UpperArg || !LowerArg) {
    Usage();
    return 2;
  }

  if (!NormalizeRootDir(UpperArg, g_UpperRoot, MAX_PATH)) {
    fwprintf(stderr, L"无效的 upper 目录\n");
    return 1;
  }
  {
    WCHAR Probe[MAX_PATH + 16];
    swprintf_s(Probe, MAX_PATH + 16, L"%s\\__ovlfs_probe__", g_UpperRoot);
    HANDLE h = CreateFileW(Probe, GENERIC_WRITE, 0, 0, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY, 0);
    if (h == INVALID_HANDLE_VALUE) {
      fwprintf(stderr, L"upper 目录不存在或不可写: %s\n", g_UpperRoot);
      return 1;
    }
    CloseHandle(h);
    DeleteFileW(Probe);
  }

  WCHAR Lowers[MAX_PATH * MAX_LOWER_LAYERS];
  wcscpy_s(Lowers, MAX_PATH * MAX_LOWER_LAYERS, LowerArg);
  WCHAR *CtxTok = 0;
  for (WCHAR *Tok = wcstok_s(Lowers, L";", &CtxTok); Tok;
       Tok = wcstok_s(0, L";", &CtxTok)) {
    if (g_LowerCount >= MAX_LOWER_LAYERS)
      break;
    if (!NormalizeRootDir(Tok, g_LowerRoots[g_LowerCount], MAX_PATH)) {
      fwprintf(stderr, L"无效的 lower 目录: %s\n", Tok);
      return 1;
    }
    if (GetFileAttributesW(g_LowerRoots[g_LowerCount]) ==
        INVALID_FILE_ATTRIBUTES) {
      fwprintf(stderr, L"lower 目录不存在: %s\n", g_LowerRoots[g_LowerCount]);
      return 1;
    }
    g_LowerCount++;
  }
  if (g_LowerCount == 0) {
    fwprintf(stderr, L"至少需要一个 lower 目录\n");
    return 1;
  }

  InitializeCriticalSection(&g_CopyUpCs);
  g_StopEvent = CreateEventW(0, TRUE, FALSE, 0);

  /* 卷参数: 面向性能, 关闭一切安全/高级特性 */
  FSP_FSCTL_VOLUME_PARAMS Vp;
  memset(&Vp, 0, sizeof Vp);
  Vp.Version = sizeof Vp;
  Vp.SectorSize = 512;
  Vp.SectorsPerAllocationUnit = 8; /* 4KB 分配单元 */
  Vp.MaxComponentLength = 255;
  GetSystemTimeAsFileTime((FILETIME *)&Vp.VolumeCreationTime);
  Vp.VolumeSerialNumber =
      (UINT32)(GetTickCount64() >> 8) ^ GetCurrentProcessId();
  Vp.FileInfoTimeout = InfoTimeout; /* 内核元数据缓存 */
  Vp.CaseSensitiveSearch = 0;
  Vp.CasePreservedNames = 1;
  Vp.UnicodeOnDisk = 1;
  Vp.PersistentAcls = 0; /* 不做 ACL */
  Vp.ReparsePoints = 0;
  Vp.ReparsePointsAccessCheck = 0;
  Vp.NamedStreams = 0;
  Vp.HardLinks = 0;
  Vp.ExtendedAttributes = 0;
  Vp.ReadOnlyVolume = 0;
  Vp.PostCleanupWhenModifiedOnly = 1; /* 减少无用 Cleanup */
  Vp.PassQueryDirectoryPattern = 0;
  Vp.PassQueryDirectoryFileName = 1; /* 启用 GetDirInfoByName 快路径 */
  Vp.FlushAndPurgeOnCleanup = 0;
  Vp.DeviceControl = 0;
  Vp.UmFileContextIsUserContext2 = 1; /* 每打开实例一个描述符 */
  Vp.AllowOpenInKernelMode = 0;
  Vp.PostDispositionWhenNecessaryOnly = 1;
  wcscpy_s(Vp.FileSystemName,
           sizeof Vp.FileSystemName / sizeof Vp.FileSystemName[0], L"OVLFS");

  static FSP_FILE_SYSTEM_INTERFACE Iface;
  memset(&Iface, 0, sizeof Iface);
  Iface.GetVolumeInfo = OvlGetVolumeInfo;
  Iface.SetVolumeLabel = OvlSetVolumeLabel;
  Iface.GetSecurityByName = OvlGetSecurityByName;
  Iface.Create = OvlCreate;
  Iface.Open = OvlOpen;
  Iface.Overwrite = OvlOverwrite;
  Iface.Cleanup = OvlCleanup;
  Iface.Close = OvlClose;
  Iface.Read = OvlRead;
  Iface.Write = OvlWrite;
  Iface.Flush = OvlFlush;
  Iface.GetFileInfo = OvlGetFileInfo;
  Iface.SetBasicInfo = OvlSetBasicInfo;
  Iface.SetFileSize = OvlSetFileSize;
  Iface.Rename = OvlRename;
  Iface.ReadDirectory = OvlReadDirectory;
  Iface.GetDirInfoByName = OvlGetDirInfoByName;
  Iface.DispatcherStopped = OvlDispatcherStopped;

  FSP_FILE_SYSTEM *FileSystem;
  NTSTATUS R = FspFileSystemPreflight(OVL_DISK_DEVICE_NAME, MountPoint);
  if (!NT_SUCCESS(R)) {
    fwprintf(stderr, L"Preflight 失败: %08lX\n", R);
    return 1;
  }
  R = FspFileSystemCreate(OVL_DISK_DEVICE_NAME, &Vp, &Iface, &FileSystem);
  if (!NT_SUCCESS(R)) {
    fwprintf(stderr, L"Create 失败: %08lX\n", R);
    return 1;
  }
  R = FspFileSystemSetMountPoint(FileSystem, MountPoint);
  if (!NT_SUCCESS(R)) {
    fwprintf(stderr, L"挂载失败: %08lX\n", R);
    return 1;
  }
  R = FspFileSystemStartDispatcher(FileSystem, Threads);
  if (!NT_SUCCESS(R)) {
    fwprintf(stderr, L"启动分发器失败: %08lX\n", R);
    return 1;
  }
  if (DebugLog)
    FspFileSystemSetDebugLog(FileSystem, DebugLog);

  SetConsoleCtrlHandler(CtrlHandler, TRUE);
  wprintf(L"OVLFS 已挂载: %s (upper=%s, %lu 个 lower 层) -- Ctrl+C 卸载退出\n",
          FspFileSystemMountPoint(FileSystem), g_UpperRoot, g_LowerCount);
  fflush(stdout);

  WaitForSingleObject(g_StopEvent, INFINITE);

  FspFileSystemStopDispatcher(FileSystem);
  FspFileSystemRemoveMountPoint(FileSystem);
  FspFileSystemDelete(FileSystem);
  wprintf(L"OVLFS 已卸载\n");
  return 0;
}
