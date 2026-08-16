/*
 * ovlfs.c - WinFsp OverlayFS（upper 可写层 + lower 只读层）
 *
 * 语义:
 *   - 读: upper 优先, 其次 lower
 *   - 写: 首次以写方式打开时 copy-up 到 upper; 之后所有修改都在 upper
 *   - 删除/改名: lower 层条目用 upper 层中的 "<whiteout><名字>" 隐藏
 *   - 不实现: ACL/安全描述符(NULL DACL)、EA、命名流、重解析点、硬链接
 *
 * 编译 (VS 开发者命令行):
 *   cl /O2 /W3 /I "%ProgramFiles(x86)%\WinFsp\inc" ovlfs.c /link
 * /LIBPATH:"%ProgramFiles(x86)%\WinFsp\lib" winfsp-x64.lib
 *
 * 运行:
 *   ovlfs -u C:\upper -l C:\lower -m O: [-i 1000] [-t 0] [-D 1]
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

#define OVL_NAME L"OverlayFS v0.0.0"
#define OVL_DISK_DEVICE_NAME L"WinFsp.Disk"

#define WHITEOUT_PREFIX L"\uF03A\uF02E"
#define WHITEOUT_PREFIX_LEN 2
#define COPY_BUF_SIZE (1024 * 1024)
#define ALLOC_UNIT ((UINT64)512 * 8)

/* ------------------------------------------------------------------ */
/* 全局状态                                                            */
/* ------------------------------------------------------------------ */

static WCHAR g_UpperRoot[MAX_PATH];
static WCHAR g_LowerRoot[MAX_PATH];
static WCHAR g_Label[32] = L"OverlayFS";
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

/* 将 Win32 错误代码转换为 NTSTATUS 状态码 */
static NTSTATUS W32Err(DWORD e) { return FspNtStatusFromWin32(e); }

/* 复制宽字符串 */
static PWSTR StrDup(PCWSTR s) {
  SIZE_T n = (wcslen(s) + 1) * sizeof(WCHAR);
  PWSTR p = malloc(n);
  if (p)
    memcpy(p, s, n);
  return p;
}

/* 拼接两个宽字符串 */
static PWSTR StrCat2(PCWSTR a, PCWSTR b) {
  SIZE_T la = wcslen(a), lb = wcslen(b);
  PWSTR p = malloc((la + lb + 1) * sizeof(WCHAR));
  if (p) {
    memcpy(p, a, la * sizeof(WCHAR));
    memcpy(p + la, b, (lb + 1) * sizeof(WCHAR));
  }
  return p;
}

/* 拼接三个宽字符串 */
static PWSTR StrCat3(PCWSTR a, PCWSTR b, PCWSTR c) {
  SIZE_T la = wcslen(a), lb = wcslen(b), lc = wcslen(c);
  PWSTR p = malloc((la + lb + lc + 1) * sizeof(WCHAR));
  if (p) {
    memcpy(p, a, la * sizeof(WCHAR));
    memcpy(p + la, b, lb * sizeof(WCHAR));
    memcpy(p + la + lb, c, (lc + 1) * sizeof(WCHAR));
  }
  return p;
}

/* 规范化根目录路径 (转为全路径、大写盘符、去除尾部反斜杠) */
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

/* 释放文件描述符及其关联的资源 */
static VOID FreeDesc(OVL_DESC *Desc) {
  if (Desc->Handle != INVALID_HANDLE_VALUE)
    CloseHandle(Desc->Handle);
  FspFileSystemDeleteDirectoryBuffer(&Desc->DirBuffer);
  free(Desc->RelName);
  free(Desc->UpperPath);
  free(Desc->LowerPath);
  free(Desc);
}

/* 分配并初始化一个新的文件描述符 */
static OVL_DESC *AllocDesc(PCWSTR Rel, PWSTR Upper, PWSTR Lower, HANDLE Handle,
                           BOOLEAN IsDir, BOOLEAN UpExists, BOOLEAN LoExists) {
  OVL_DESC *d = calloc(1, sizeof *d);
  PWSTR RelName = StrDup(Rel);
  if (!d || !RelName) {
    free(d);
    free(RelName);
    free(Upper);
    free(Lower);
    return 0;
  }
  d->Handle = Handle;
  d->RelName = RelName;
  d->UpperPath = Upper;
  d->LowerPath = Lower;
  d->IsDirectory = IsDir;
  d->UpperExists = UpExists;
  d->LowerExists = LoExists;
  return d;
}

/* 从句柄获取文件信息并填充到 FSP_FSCTL_FILE_INFO 结构 */
static NTSTATUS GetFileInfoFromHandle(HANDLE h, FSP_FSCTL_FILE_INFO *Fi) {
  BY_HANDLE_FILE_INFORMATION Bh;
  memset(Fi, 0, sizeof *Fi);
  if (!GetFileInformationByHandle(h, &Bh))
    return W32Err(GetLastError());
  UINT64 Size = ((UINT64)Bh.nFileSizeHigh << 32) | Bh.nFileSizeLow;
  Fi->FileAttributes = Bh.dwFileAttributes;
  Fi->FileSize = Size;
  Fi->AllocationSize = (Size + g_AllocUnit - 1) & ~(g_AllocUnit - 1);
  Fi->CreationTime = *(UINT64 *)&Bh.ftCreationTime;
  Fi->LastAccessTime = *(UINT64 *)&Bh.ftLastAccessTime;
  Fi->LastWriteTime = *(UINT64 *)&Bh.ftLastWriteTime;
  Fi->ChangeTime = *(UINT64 *)&Bh.ftLastWriteTime;
  Fi->IndexNumber = ((UINT64)Bh.nFileIndexHigh << 32) | Bh.nFileIndexLow;
  return STATUS_SUCCESS;
}

/* 从 WIN32_FIND_DATAW 获取文件信息并填充到 FSP_FSCTL_FILE_INFO 结构 */
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

/* 打开底层 NTFS 文件或目录句柄 */
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

/* ------------------------------------------------------------------ */
/* 合并视图查找 / whiteout                                             */
/* ------------------------------------------------------------------ */

/* 在 lower 层查找指定相对路径，若存在则返回其全路径 */
static BOOLEAN FindLowerPath(PCWSTR Rel, PWSTR *Path) {
  PWSTR p = StrCat2(g_LowerRoot, Rel);
  if (p && GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) {
    *Path = p;
    return TRUE;
  }
  free(p);
  return FALSE;
}

/* 构造指定相对路径对应的 whiteout 文件全路径 */
static PWSTR WhiteoutPathOf(PCWSTR Rel) {
  PCWSTR Slash = wcsrchr(Rel, L'\\');
  if (!Slash)
    return 0;
  SIZE_T ParentLen = Slash - Rel;
  PCWSTR Leaf = Slash + 1;
  SIZE_T Total = wcslen(g_UpperRoot) + ParentLen + 1 + WHITEOUT_PREFIX_LEN +
                 wcslen(Leaf) + 1;
  PWSTR p = malloc(Total * sizeof(WCHAR));
  if (!p)
    return 0;
  wcscpy_s(p, Total, g_UpperRoot);
  wcsncat_s(p, Total, Rel, ParentLen);
  wcscat_s(p, Total, L"\\" WHITEOUT_PREFIX);
  wcscat_s(p, Total, Leaf);
  return p;
}

/* 检查指定相对路径是否存在 whiteout 标记 */
static BOOLEAN WhiteoutExistsAt(PCWSTR Rel) {
  PWSTR wp = WhiteoutPathOf(Rel);
  if (!wp)
    return FALSE;
  BOOLEAN b = GetFileAttributesW(wp) != INVALID_FILE_ATTRIBUTES;
  free(wp);
  return b;
}

/* 在合并视图 (upper 优先，其次 lower) 中查找文件属性 */
static BOOLEAN MergedLookup(PCWSTR Rel, PUINT32 PAttr) {
  PWSTR Upper = StrCat2(g_UpperRoot, Rel);
  if (Upper) {
    DWORD a = GetFileAttributesW(Upper);
    free(Upper);
    if (a != INVALID_FILE_ATTRIBUTES) {
      *PAttr = a;
      return TRUE;
    }
  }
  if (WhiteoutExistsAt(Rel))
    return FALSE;
  PWSTR Lower = StrCat2(g_LowerRoot, Rel);
  if (Lower) {
    DWORD a = GetFileAttributesW(Lower);
    free(Lower);
    if (a != INVALID_FILE_ATTRIBUTES) {
      *PAttr = a;
      return TRUE;
    }
  }
  return FALSE;
}

/* ------------------------------------------------------------------ */
/* upper 父目录惰性 copy-up                                            */
/* ------------------------------------------------------------------ */

/* 复制文件的元数据 (如属性、时间戳) 从源路径到目标路径 */
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

/* 确保 upper 层存在指定的目录，若不存在则从 lower 层 copy-up */
static NTSTATUS EnsureUpperDir(PCWSTR Rel) {
  PWSTR Upper = StrCat2(g_UpperRoot, Rel);
  if (!Upper)
    return STATUS_NO_MEMORY;
  if (GetFileAttributesW(Upper) != INVALID_FILE_ATTRIBUTES) {
    free(Upper);
    return STATUS_SUCCESS;
  }
  PWSTR Lower;
  if (!FindLowerPath(Rel, &Lower)) {
    free(Upper);
    return STATUS_OBJECT_PATH_NOT_FOUND;
  }
  if (!CreateDirectoryW(Upper, 0) && GetLastError() != ERROR_ALREADY_EXISTS) {
    NTSTATUS R = W32Err(GetLastError());
    free(Upper);
    free(Lower);
    return R;
  }
  CopyFileMeta(Lower, Upper);
  free(Upper);
  free(Lower);
  return STATUS_SUCCESS;
}

/* 确保 upper 层存在指定路径的所有父目录 */
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

/* 底层文件数据流复制 (用于 copy-up) */
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
        if (Wr == 0) {
          R = STATUS_UNEXPECTED_IO_ERROR;
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

/* 将单个文件或目录从 lower 层 copy-up 到 upper 层 */
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

/* 释放 copy-up 引擎的引用计数并清理资源 */
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

/* 并发安全的 copy-up 执行函数 (同路径并发去重) */
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
    PWSTR Path = StrDup(Upper);
    if (!Cu || !Path) {
      free(Cu);
      free(Path);
      LeaveCriticalSection(&g_CopyUpCs);
      return STATUS_NO_MEMORY;
    }
    Cu->Path = Path;
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

/* 确保描述符指向的文件在 upper 层可写 (必要时触发 copy-up 并替换句柄) */
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

/* 创建 whiteout 文件以在 upper 层隐藏 lower 层的同名条目 */
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

/* 递归删除 upper 目录树中的所有 whiteout 文件或所有文件 */
static VOID PurgeWhiteoutsRecursive(PCWSTR DirPath, BOOLEAN All) {
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
    if (Fd.cFileName[0] == L'.' &&
        (Fd.cFileName[1] == L'\0' ||
         (Fd.cFileName[1] == L'.' && Fd.cFileName[2] == L'\0')))
      continue;
    PWSTR Full = StrCat3(DirPath, L"\\", Fd.cFileName);
    if (!Full)
      continue;
    if (Fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      PurgeWhiteoutsRecursive(Full, All);
    else if (All ||
             wcsncmp(Fd.cFileName, WHITEOUT_PREFIX, WHITEOUT_PREFIX_LEN) == 0) {
      SetFileAttributesW(Full, FILE_ATTRIBUTE_NORMAL);
      DeleteFileW(Full);
    }
    free(Full);
  } while (FindNextFileW(F, &Fd));
  FindClose(F);
  if (All)
    RemoveDirectoryW(DirPath);
}

/* 移除指定路径的 whiteout 文件 */
static VOID RemoveWhiteout(PCWSTR Rel) {
  PWSTR wp = WhiteoutPathOf(Rel);
  if (!wp)
    return;

  DWORD attr = GetFileAttributesW(wp);
  if (attr == INVALID_FILE_ATTRIBUTES) {
  } else if (attr & FILE_ATTRIBUTE_DIRECTORY) {
    PurgeWhiteoutsRecursive(wp, TRUE);
  } else {
    DeleteFileW(wp);
  }
  free(wp);
}

/* 递归在 upper 层创建 whiteout 以隐藏 lower 层的目录内容 */
static NTSTATUS CreateWhiteoutsRecursive(PCWSTR Rel, PCWSTR Upper,
                                         PCWSTR Lower) {
  DWORD LowerAttr = GetFileAttributesW(Lower);
  if (LowerAttr == INVALID_FILE_ATTRIBUTES)
    return W32Err(GetLastError());
  DWORD UpperAttr = GetFileAttributesW(Upper);
  if (UpperAttr == INVALID_FILE_ATTRIBUTES) {
    MakeWhiteout(Rel);
    return STATUS_SUCCESS;
  } else if (!(UpperAttr & FILE_ATTRIBUTE_DIRECTORY))
    return STATUS_SUCCESS;

  PWSTR Pat = StrCat2(Lower, L"\\*");
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
    if (Fd.cFileName[0] == L'.' &&
        (Fd.cFileName[1] == L'\0' ||
         (Fd.cFileName[1] == L'.' && Fd.cFileName[2] == L'\0')))
      continue;
    PWSTR ChildRel = StrCat3(Rel, L"\\", Fd.cFileName);
    PWSTR ChildUpper = StrCat3(Upper, L"\\", Fd.cFileName);
    PWSTR ChildLower = StrCat3(Lower, L"\\", Fd.cFileName);
    if (!ChildRel || !ChildUpper || !ChildLower)
      R = STATUS_NO_MEMORY;
    else
      R = CreateWhiteoutsRecursive(ChildRel, ChildUpper, ChildLower);

    free(ChildRel);
    free(ChildUpper);
    free(ChildLower);
    if (!NT_SUCCESS(R))
      break;
  } while (FindNextFileW(F, &Fd));
  FindClose(F);
  return R;
}

/* ------------------------------------------------------------------ */
/* 名字哈希集合 (目录合并去重)                                         */
/* ------------------------------------------------------------------ */

typedef struct _NAMESET {
  PWSTR *Items;
  ULONG Count, Capacity;
} NAMESET;

/* 计算字符串的 FNV-1a 哈希值 (忽略大小写) */
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

/* 初始化名称哈希集合 (用于目录合并去重) */
static BOOLEAN NameSetInit(NAMESET *Set, ULONG Cap) {
  Set->Count = 0;
  Set->Capacity = Cap;
  Set->Items = calloc(Cap, sizeof(PWSTR));
  return Set->Items != 0;
}

/* 释放名称哈希集合占用的内存 */
static VOID NameSetFree(NAMESET *Set) {
  for (ULONG i = 0; i < Set->Capacity; i++)
    free(Set->Items[i]);
  free(Set->Items);
}

/* 向哈希集合中插入名称，返回 0 表示成功，1 表示已存在，-1 表示内存分配失败 */
static char NameSetInsert(NAMESET *Set, PCWSTR Name) {
  if ((Set->Count + 1) * 10 >= Set->Capacity * 7) {
    ULONG NewCap = Set->Capacity * 2;
    PWSTR *New = calloc(NewCap, sizeof(PWSTR));
    if (!New)
      return -1;
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
      PWSTR Dup = StrDup(Name);
      if (!Dup)
        return -1;
      Set->Items[i] = Dup;
      Set->Count++;
      return 0;
    }
    if (_wcsicmp(e, Name) == 0)
      return 1;
    i = (i + 1) & (Set->Capacity - 1);
  }
}

/* ------------------------------------------------------------------ */
/* 文件系统操作实现                                                    */
/* ------------------------------------------------------------------ */

/* 获取卷信息 (总大小、可用空间、卷标等) */
static NTSTATUS OvlGetVolumeInfo(FSP_FILE_SYSTEM *Fs,
                                 FSP_FSCTL_VOLUME_INFO *VolumeInfo) {
  (void)Fs;
  ULARGE_INTEGER FreeAvail, Total, TotalFree;
  if (!GetDiskFreeSpaceExW(g_UpperRoot, &FreeAvail, &Total, &TotalFree))
    return W32Err(GetLastError());
  memset(VolumeInfo, 0, sizeof *VolumeInfo);
  VolumeInfo->TotalSize = Total.QuadPart;
  VolumeInfo->FreeSize = FreeAvail.QuadPart;
  VolumeInfo->VolumeLabelLength = (UINT16)(wcslen(g_Label) * sizeof(WCHAR));
  memcpy(VolumeInfo->VolumeLabel, g_Label, VolumeInfo->VolumeLabelLength);
  return STATUS_SUCCESS;
}

/* 设置卷标 */
static NTSTATUS OvlSetVolumeLabel(FSP_FILE_SYSTEM *Fs, PWSTR VolumeLabel,
                                  FSP_FSCTL_VOLUME_INFO *VolumeInfo) {
  (void)Fs;
  wcsncpy_s(g_Label, 32, VolumeLabel, _TRUNCATE);
  return OvlGetVolumeInfo(0, VolumeInfo);
}

/* 判断文件或目录是否可以被删除（目录需检查合并视图是否为空） */
static NTSTATUS OvlCanDelete(FSP_FILE_SYSTEM *Fs, PVOID FileContext,
                             PCWSTR FileName) {
  (void)Fs;
  (void)FileName;
  OVL_DESC *Desc = FileContext;

  if (!Desc->IsDirectory)
    return STATUS_SUCCESS;

  /* 检查 upper 层是否有可见条目（排除 whiteout 文件） */
  if (Desc->UpperExists) {
    PWSTR Pat = StrCat2(Desc->UpperPath, L"\\*");
    if (!Pat)
      return STATUS_NO_MEMORY;
    WIN32_FIND_DATAW Fd;
    HANDLE F = FindFirstFileExW(Pat, FindExInfoBasic, &Fd,
                                FindExSearchNameMatch, 0, 0);
    free(Pat);
    if (F != INVALID_HANDLE_VALUE) {
      do {
        if (Fd.cFileName[0] == L'.' &&
            (Fd.cFileName[1] == L'\0' ||
             (Fd.cFileName[1] == L'.' && Fd.cFileName[2] == L'\0')))
          continue;
        if (wcsncmp(Fd.cFileName, WHITEOUT_PREFIX, WHITEOUT_PREFIX_LEN) == 0)
          continue;
        FindClose(F);
        return STATUS_DIRECTORY_NOT_EMPTY;
      } while (FindNextFileW(F, &Fd));
      FindClose(F);
    }
  }

  /* 检查 lower 层是否有未被 whiteout 隐藏的条目 */
  if (Desc->LowerExists && Desc->LowerPath) {
    PWSTR Pat = StrCat2(Desc->LowerPath, L"\\*");
    if (!Pat)
      return STATUS_NO_MEMORY;
    WIN32_FIND_DATAW Fd;
    HANDLE F = FindFirstFileExW(Pat, FindExInfoBasic, &Fd,
                                FindExSearchNameMatch, 0, 0);
    free(Pat);
    if (F != INVALID_HANDLE_VALUE) {
      do {
        if (Fd.cFileName[0] == L'.' &&
            (Fd.cFileName[1] == L'\0' ||
             (Fd.cFileName[1] == L'.' && Fd.cFileName[2] == L'\0')))
          continue;
        PWSTR ChildRel = StrCat3(Desc->RelName, L"\\", Fd.cFileName);
        if (!ChildRel) {
          FindClose(F);
          return STATUS_NO_MEMORY;
        }
        BOOLEAN Hidden = WhiteoutExistsAt(ChildRel);
        free(ChildRel);
        if (!Hidden) {
          FindClose(F);
          return STATUS_DIRECTORY_NOT_EMPTY;
        }
      } while (FindNextFileW(F, &Fd));
      FindClose(F);
    }
  }

  return STATUS_SUCCESS;
}

/* 获取文件的安全描述符和属性 (仅返回 NULL DACL) */
static NTSTATUS OvlGetSecurityByName(FSP_FILE_SYSTEM *Fs, PCWSTR FileName,
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
    *PSecurityDescriptorSize = 0;
  return STATUS_SUCCESS;
}

/* 创建新文件或目录 */
static NTSTATUS OvlCreate(FSP_FILE_SYSTEM *Fs, PCWSTR FileName,
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

  PWSTR Upper = StrCat2(g_UpperRoot, FileName);
  if (!Upper)
    return STATUS_NO_MEMORY;

  HANDLE h;
  if (IsDir) {
    if (!CreateDirectoryW(Upper, 0)) {
      R = W32Err(GetLastError());
      free(Upper);
      return R;
    }
    if (FileAttributes)
      SetFileAttributesW(Upper, FileAttributes);
    h = OpenUnderlying(Upper, TRUE, TRUE, CreateOptions);
  } else {
    h = CreateFileW(Upper, GENERIC_READ | GENERIC_WRITE,
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
    free(Upper);
    return R;
  }

  RemoveWhiteout(FileName);

  PWSTR Lower = 0;
  BOOLEAN LowerExists = FindLowerPath(FileName, &Lower);
  if (IsDir && LowerExists)
    CreateWhiteoutsRecursive(FileName, Upper, Lower);

  OVL_DESC *d = AllocDesc(FileName, Upper, Lower, h, IsDir, TRUE, LowerExists);
  if (!d) {
    CloseHandle(h);
    return STATUS_NO_MEMORY;
  }
  *PFileContext = d;
  return GetFileInfoFromHandle(h, FileInfo);
}

/* 打开现有文件或目录 (处理只读透传和写时复制) */
static NTSTATUS OvlOpen(FSP_FILE_SYSTEM *Fs, PCWSTR FileName,
                        UINT32 CreateOptions, UINT32 GrantedAccess,
                        PVOID *PFileContext, FSP_FSCTL_FILE_INFO *FileInfo) {
  (void)Fs;
  BOOLEAN Writable =
      (GrantedAccess & (FILE_WRITE_DATA | FILE_APPEND_DATA |
                        FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA)) != 0;
  PWSTR Upper = StrCat2(g_UpperRoot, FileName);
  if (!Upper)
    return STATUS_NO_MEMORY;

  DWORD UpperAttr = GetFileAttributesW(Upper);
  BOOLEAN UpperExists = UpperAttr != INVALID_FILE_ATTRIBUTES;
  PWSTR Lower = 0;
  BOOLEAN LowerExists = FALSE;
  HANDLE h = INVALID_HANDLE_VALUE;
  BOOLEAN Copied = FALSE;
  NTSTATUS R;

  if (UpperExists) {
    h = OpenUnderlying(Upper, (UpperAttr & FILE_ATTRIBUTE_DIRECTORY) != 0,
                       Writable, CreateOptions);
    if (h == INVALID_HANDLE_VALUE) {
      R = W32Err(GetLastError());
      free(Upper);
      return R;
    }
    LowerExists = FindLowerPath(FileName, &Lower);
  } else {
    if (WhiteoutExistsAt(FileName)) {
      free(Upper);
      return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    if (!FindLowerPath(FileName, &Lower)) {
      free(Upper);
      return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    LowerExists = TRUE;
    BOOLEAN IsDir = (GetFileAttributesW(Lower) & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (Writable) {
      /* 写打开 => 立即 copy-up */
      R = EnsureUpperParents(FileName);
      if (NT_SUCCESS(R))
        R = CopyUpPath(Lower, Upper);
      if (!NT_SUCCESS(R)) {
        free(Upper);
        free(Lower);
        return R;
      }
      h = OpenUnderlying(Upper, IsDir, TRUE, CreateOptions);
      if (h == INVALID_HANDLE_VALUE) {
        R = W32Err(GetLastError());
        free(Upper);
        free(Lower);
        return R;
      }
      RemoveWhiteout(FileName);
      Copied = TRUE;
    } else {
      /* 只读打开 => 直接透传 lower 层句柄, 零拷贝 */
      h = OpenUnderlying(Lower, IsDir, FALSE, CreateOptions);
      if (h == INVALID_HANDLE_VALUE) {
        R = W32Err(GetLastError());
        free(Upper);
        free(Lower);
        return R;
      }
    }
  }

  BOOLEAN IsDir2 = FALSE;
  BY_HANDLE_FILE_INFORMATION Bh;
  if (GetFileInformationByHandle(h, &Bh))
    IsDir2 = (Bh.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

  OVL_DESC *d = AllocDesc(FileName, Upper, Lower, h, IsDir2,
                          UpperExists || Copied, LowerExists);
  if (!d) {
    CloseHandle(h);
    return STATUS_NO_MEMORY;
  }
  *PFileContext = d;
  return GetFileInfoFromHandle(h, FileInfo);
}

/* 覆盖现有文件 (截断并可选设置属性) */
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
    FILE_END_OF_FILE_INFO EofInfo;
    EofInfo.EndOfFile.QuadPart = 0;
    if (!SetFileInformationByHandle(Desc->Handle, FileEndOfFileInfo, &EofInfo,
                                    sizeof(EofInfo)))
      return W32Err(GetLastError());
  }

  if (AllocationSize) {
    FILE_ALLOCATION_INFO AllocInfo;
    AllocInfo.AllocationSize.QuadPart = (LONGLONG)AllocationSize;
    SetFileInformationByHandle(Desc->Handle, FileAllocationInfo, &AllocInfo,
                               sizeof(AllocInfo));
  }

  if (FileAttributes) {
    DWORD cur = GetFileAttributesW(Desc->UpperPath);
    if (cur != INVALID_FILE_ATTRIBUTES)
      SetFileAttributesW(Desc->UpperPath, ReplaceFileAttributes
                                              ? FileAttributes
                                              : (cur | FileAttributes));
  }

  return GetFileInfoFromHandle(Desc->Handle, FileInfo);
}

/* 清理文件上下文 (处理删除操作、whiteout 创建及底层 NTFS 维护) */
static VOID OvlCleanup(FSP_FILE_SYSTEM *Fs, PVOID FileContext, PCWSTR FileName,
                       ULONG Flags) {
  (void)Fs;
  (void)FileName;
  OVL_DESC *Desc = FileContext;
  if (!(Flags & FspCleanupDelete))
    return; /* 时间/归档位等由底层 NTFS 自行维护 */

  if (Desc->UpperExists) {
    if (Desc->IsDirectory)
      PurgeWhiteoutsRecursive(Desc->UpperPath, TRUE);
    else
      DeleteFileW(Desc->UpperPath);
  }
  if (Desc->LowerExists)
    MakeWhiteout(Desc->RelName);
}

/* 关闭文件并释放描述符资源 */
static VOID OvlClose(FSP_FILE_SYSTEM *Fs, PVOID FileContext) {
  (void)Fs;
  FreeDesc(FileContext);
}

/* 读取文件数据 */
static NTSTATUS OvlRead(FSP_FILE_SYSTEM *Fs, PVOID FileContext, PVOID Buffer,
                        UINT64 Offset, ULONG Length, PULONG PBytesTransferred) {
  (void)Fs;
  OVL_DESC *Desc = FileContext;
  OVERLAPPED Ov = {0};
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

/* 写入文件数据 */
static NTSTATUS OvlWrite(FSP_FILE_SYSTEM *Fs, PVOID FileContext, PVOID Buffer,
                         UINT64 Offset, ULONG Length, BOOLEAN WriteToEndOfFile,
                         BOOLEAN ConstrainedIo, PULONG PBytesTransferred,
                         FSP_FSCTL_FILE_INFO *FileInfo) {
  (void)Fs;
  OVL_DESC *Desc = FileContext;
  *PBytesTransferred = 0;
  if (Length == 0)
    return GetFileInfoFromHandle(Desc->Handle, FileInfo);

  if (WriteToEndOfFile) {
    LARGE_INTEGER fs;
    if (!GetFileSizeEx(Desc->Handle, &fs))
      return W32Err(GetLastError());
    Offset = (UINT64)fs.QuadPart;
  } else if (ConstrainedIo) {
    LARGE_INTEGER fs;
    if (!GetFileSizeEx(Desc->Handle, &fs))
      return W32Err(GetLastError());
    if (Offset >= (UINT64)fs.QuadPart)
      return GetFileInfoFromHandle(Desc->Handle, FileInfo);
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
  return GetFileInfoFromHandle(Desc->Handle, FileInfo);
}

/* 刷新文件缓冲区到磁盘 */
static NTSTATUS OvlFlush(FSP_FILE_SYSTEM *Fs, PVOID FileContext,
                         FSP_FSCTL_FILE_INFO *FileInfo) {
  (void)Fs;
  if (!FileContext)
    return STATUS_SUCCESS; /* 卷刷新: FSD 已刷缓存 */
  OVL_DESC *Desc = FileContext;
  if (!FlushFileBuffers(Desc->Handle))
    return W32Err(GetLastError());
  return GetFileInfoFromHandle(Desc->Handle, FileInfo);
}

/* 获取文件基本信息 */
static NTSTATUS OvlGetFileInfo(FSP_FILE_SYSTEM *Fs, PVOID FileContext,
                               FSP_FSCTL_FILE_INFO *FileInfo) {
  (void)Fs;
  return GetFileInfoFromHandle(((OVL_DESC *)FileContext)->Handle, FileInfo);
}

/* 设置文件基本属性 (时间戳、文件属性) */
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
  return GetFileInfoFromHandle(Desc->Handle, FileInfo);
}

/* 设置文件大小或分配大小 */
static NTSTATUS OvlSetFileSize(FSP_FILE_SYSTEM *Fs, PVOID FileContext,
                               UINT64 NewSize, BOOLEAN SetAllocationSize,
                               FSP_FSCTL_FILE_INFO *FileInfo) {
  (void)Fs;
  OVL_DESC *Desc = FileContext;
  NTSTATUS R = EnsureUpperWritable(Desc);
  if (!NT_SUCCESS(R))
    return R;

  if (SetAllocationSize) {
    FILE_ALLOCATION_INFO AllocInfo;
    AllocInfo.AllocationSize.QuadPart = (LONGLONG)NewSize;
    if (!SetFileInformationByHandle(Desc->Handle, FileAllocationInfo,
                                    &AllocInfo, sizeof(AllocInfo)))
      return W32Err(GetLastError());
  } else {
    FILE_END_OF_FILE_INFO EofInfo;
    EofInfo.EndOfFile.QuadPart = (LONGLONG)NewSize;
    if (!SetFileInformationByHandle(Desc->Handle, FileEndOfFileInfo, &EofInfo,
                                    sizeof(EofInfo)))
      return W32Err(GetLastError());
  }

  return GetFileInfoFromHandle(Desc->Handle, FileInfo);
}

/* 递归将 lower 层的目录树 copy-up 到 upper 层 (用于 Rename 前的准备) */
static NTSTATUS EnsureUpperRecursive(PCWSTR Rel) {
  PWSTR Lower = 0;
  if (!FindLowerPath(Rel, &Lower))
    return STATUS_SUCCESS;

  if (WhiteoutExistsAt(Rel)) {
    free(Lower);
    return STATUS_SUCCESS;
  }

  PWSTR Upper = StrCat2(g_UpperRoot, Rel);
  if (!Upper) {
    free(Lower);
    return STATUS_NO_MEMORY;
  }

  DWORD UpperAttr = GetFileAttributesW(Upper);
  DWORD LowerAttr = GetFileAttributesW(Lower);
  if (LowerAttr == INVALID_FILE_ATTRIBUTES) {
    free(Upper);
    free(Lower);
    return W32Err(GetLastError());
  }
  if (UpperAttr == INVALID_FILE_ATTRIBUTES) {
    if (!(LowerAttr & FILE_ATTRIBUTE_DIRECTORY)) {
      NTSTATUS R = CopyUpPath(Lower, Upper);
      free(Upper);
      free(Lower);
      return R;
    }
  } else {
    if (!(UpperAttr & FILE_ATTRIBUTE_DIRECTORY) ||
        !(LowerAttr & FILE_ATTRIBUTE_DIRECTORY)) {
      free(Upper);
      free(Lower);
      return STATUS_SUCCESS;
    }
  }

  NTSTATUS R = CopyUpPath(Lower, Upper);
  if (!NT_SUCCESS(R)) {
    free(Upper);
    free(Lower);
    return R;
  }

  PWSTR Pat = StrCat2(Lower, L"\\*");
  if (!Pat) {
    free(Upper);
    free(Lower);
    return STATUS_NO_MEMORY;
  }
  WIN32_FIND_DATAW Fd;
  HANDLE F = FindFirstFileExW(Pat, FindExInfoBasic, &Fd, FindExSearchNameMatch,
                              0, FIND_FIRST_EX_LARGE_FETCH);
  free(Pat);
  if (F == INVALID_HANDLE_VALUE) {
    free(Upper);
    free(Lower);
    return W32Err(GetLastError());
  }

  do {
    if (Fd.cFileName[0] == L'.' &&
        (Fd.cFileName[1] == L'\0' ||
         (Fd.cFileName[1] == L'.' && Fd.cFileName[2] == L'\0')))
      continue;
    PWSTR ChildRel = StrCat3(Rel, L"\\", Fd.cFileName);
    if (!ChildRel) {
      R = STATUS_NO_MEMORY;
      break;
    }
    R = EnsureUpperRecursive(ChildRel);
    free(ChildRel);
    if (!NT_SUCCESS(R))
      break;
  } while (FindNextFileW(F, &Fd));
  FindClose(F);

  free(Upper);
  free(Lower);
  return R;
}

/* 重命名文件或目录 (处理跨层移动及 whiteout 更新) */
static NTSTATUS OvlRename(FSP_FILE_SYSTEM *Fs, PVOID FileContext,
                          PCWSTR FileName, PCWSTR NewFileName,
                          BOOLEAN ReplaceIfExists) {
  (void)Fs;
  (void)FileName;
  OVL_DESC *Desc = FileContext;

  UINT32 TAttr;
  if (MergedLookup(NewFileName, &TAttr)) {
    if (Desc->IsDirectory || !ReplaceIfExists)
      return STATUS_OBJECT_NAME_COLLISION;
    else if (TAttr & FILE_ATTRIBUTE_DIRECTORY)
      return STATUS_ACCESS_DENIED;
  }

  NTSTATUS R = EnsureUpperParents(NewFileName);
  if (!NT_SUCCESS(R))
    return R;

  PWSTR NewUpper = StrCat2(g_UpperRoot, NewFileName);
  if (!NewUpper)
    return STATUS_NO_MEMORY;

  /* 先把 Lower 中混合的文件全部递归 Copy-Up 到 Upper，避免 Rename 时丢失 */
  if (Desc->LowerExists && !(Desc->UpperExists && !Desc->IsDirectory)) {
    R = EnsureUpperRecursive(Desc->RelName);
    if (!NT_SUCCESS(R)) {
      free(NewUpper);
      return R;
    }
    Desc->UpperExists = TRUE;
  }

  if (!MoveFileExW(Desc->UpperPath, NewUpper,
                   (ReplaceIfExists ? MOVEFILE_REPLACE_EXISTING : 0) |
                       MOVEFILE_WRITE_THROUGH)) {
    R = W32Err(GetLastError());
    free(NewUpper);
    return R;
  }

  if (Desc->LowerExists) {
    MakeWhiteout(Desc->RelName);
    PurgeWhiteoutsRecursive(NewUpper, FALSE);
  }
  RemoveWhiteout(NewFileName);

  free(Desc->UpperPath);
  Desc->UpperPath = NewUpper;

  PWSTR RelName = StrDup(NewFileName);
  if (!RelName)
    return STATUS_NO_MEMORY;
  free(Desc->RelName);
  Desc->RelName = RelName;

  free(Desc->LowerPath);
  Desc->LowerExists = FindLowerPath(Desc->RelName, &Desc->LowerPath);
  if (!Desc->LowerExists)
    Desc->LowerPath = 0;
  else if (Desc->IsDirectory)
    CreateWhiteoutsRecursive(Desc->RelName, Desc->UpperPath, Desc->LowerPath);

  CloseHandle(Desc->Handle);
  Desc->Handle = OpenUnderlying(NewUpper, Desc->IsDirectory, TRUE, 0);
  if (Desc->Handle == INVALID_HANDLE_VALUE)
    return W32Err(GetLastError());
  return STATUS_SUCCESS;
}

/* 枚举 upper 或 lower 层的目录内容并填充目录缓冲区 (处理 whiteout 过滤) */
static NTSTATUS OvlEnumLayer(PCWSTR DirPath, BOOLEAN IsUpper, NAMESET *Set,
                             PVOID *PDirBuf, PNTSTATUS PResult) {
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
        wcsncmp(Fd.cFileName, WHITEOUT_PREFIX, WHITEOUT_PREFIX_LEN) == 0) {
      /* whiteout: 登记被隐藏的名字, 不输出 */
      char ns = NameSetInsert(Set, Fd.cFileName + WHITEOUT_PREFIX_LEN);
      if (ns == -1) {
        R = STATUS_NO_MEMORY;
        break;
      }
      continue;
    }
    char ns = NameSetInsert(Set, Fd.cFileName);
    if (ns == -1) {
      R = STATUS_NO_MEMORY;
      break;
    } else if (ns == 1)
      continue; /* upper 或前一个 lower 优先 */

    UINT8 DiBuf[FIELD_OFFSET(FSP_FSCTL_DIR_INFO, FileNameBuf) +
                MAX_PATH * sizeof(WCHAR)];
    FSP_FSCTL_DIR_INFO *Di = (FSP_FSCTL_DIR_INFO *)DiBuf;
    memset(Di, 0, sizeof DiBuf);

    SIZE_T Len = wcslen(Fd.cFileName);
    Di->Size = (UINT16)(FIELD_OFFSET(FSP_FSCTL_DIR_INFO, FileNameBuf) +
                        Len * sizeof(WCHAR));
    GetFileInfoFromFindData(&Fd, &Di->FileInfo);
    memcpy(Di->FileNameBuf, Fd.cFileName, Len * sizeof(WCHAR));

    if (!FspFileSystemFillDirectoryBuffer(PDirBuf, Di, PResult)) {
      R = *PResult;
      break;
    }
  } while (FindNextFileW(F, &Fd));
  FindClose(F);
  return R;
}

/* 读取目录内容 (合并 upper 和 lower 层并去重) */
static NTSTATUS OvlReadDirectory(FSP_FILE_SYSTEM *Fs, PVOID FileContext,
                                 PWSTR Pattern, PWSTR Marker, PVOID Buffer,
                                 ULONG Length, PULONG PBytesTransferred) {
  (void)Fs;
  (void)Pattern; /* FSD 自行做通配符匹配 */
  OVL_DESC *Desc = FileContext;
  NTSTATUS R = STATUS_SUCCESS;
  BOOLEAN Reset = (0 == Marker);

  if (!FspFileSystemAcquireDirectoryBuffer(&Desc->DirBuffer, Reset, &R))
    return R;

  if (Reset) {
    NAMESET Set;
    if (!NameSetInit(&Set, 256)) {
      FspFileSystemReleaseDirectoryBuffer(&Desc->DirBuffer);
      return STATUS_NO_MEMORY;
    }
    if (Desc->UpperExists)
      R = OvlEnumLayer(Desc->UpperPath, TRUE, &Set, &Desc->DirBuffer, &R);
    if (NT_SUCCESS(R) && Desc->LowerExists)
      R = OvlEnumLayer(Desc->LowerPath, FALSE, &Set, &Desc->DirBuffer, &R);
    NameSetFree(&Set);
    if (!NT_SUCCESS(R)) {
      FspFileSystemReleaseDirectoryBuffer(&Desc->DirBuffer);
      return R;
    }
  }

  FspFileSystemReleaseDirectoryBuffer(&Desc->DirBuffer);
  FspFileSystemReadDirectoryBuffer(&Desc->DirBuffer, Marker, Buffer, Length,
                                   PBytesTransferred);
  return STATUS_SUCCESS;
}

/* 通过文件名获取目录项信息 (快速路径) */
static NTSTATUS OvlGetDirInfoByName(FSP_FILE_SYSTEM *Fs, PVOID FileContext,
                                    PCWSTR FileName,
                                    FSP_FSCTL_DIR_INFO *DirInfo) {
  (void)Fs;
  OVL_DESC *Parent = FileContext;
  PWSTR ChildRel = StrCat3(Parent->RelName, L"\\", FileName);
  if (!ChildRel)
    return STATUS_NO_MEMORY;

  WIN32_FIND_DATAW Fd;
  PWSTR Upper = StrCat2(g_UpperRoot, ChildRel);
  if (!Upper) {
    free(ChildRel);
    return STATUS_NO_MEMORY;
  }
  HANDLE F = FindFirstFileW(Upper, &Fd);
  free(Upper);

  if (F == INVALID_HANDLE_VALUE) {
    if (WhiteoutExistsAt(ChildRel)) {
      free(ChildRel);
      return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    PWSTR Lower = StrCat2(g_LowerRoot, ChildRel);
    if (!Lower) {
      free(ChildRel);
      return STATUS_NO_MEMORY;
    }
    F = FindFirstFileW(Lower, &Fd);
    free(Lower);

    if (F == INVALID_HANDLE_VALUE) {
      free(ChildRel);
      return W32Err(GetLastError());
    }
  }
  free(ChildRel);

  SIZE_T Len = wcslen(Fd.cFileName);
  memset(DirInfo, 0, FIELD_OFFSET(FSP_FSCTL_DIR_INFO, FileNameBuf));
  DirInfo->Size = (UINT16)(FIELD_OFFSET(FSP_FSCTL_DIR_INFO, FileNameBuf) +
                           Len * sizeof(WCHAR));
  GetFileInfoFromFindData(&Fd, &DirInfo->FileInfo);
  memcpy(DirInfo->FileNameBuf, Fd.cFileName, Len * sizeof(WCHAR));

  FindClose(F);
  return STATUS_SUCCESS;
}

/* 分发器停止时的回调函数 (触发主线程退出) */
static VOID OvlDispatcherStopped(FSP_FILE_SYSTEM *Fs, BOOLEAN Normally) {
  (void)Fs;
  (void)Normally;
  SetEvent(g_StopEvent);
}

/* ------------------------------------------------------------------ */
/* 主程序                                                              */
/* ------------------------------------------------------------------ */

/* 控制台控制事件处理函数 (处理 Ctrl+C、关闭控制台等事件) */
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

/* 打印程序使用帮助信息 */
static VOID Usage(void) {
  fwprintf(stderr, L"WinFsp " OVL_NAME L"\n"
                   L"用法: ovlfs -u <顶层目录> -l <底层目录> [选项]\n"
                   L"选项:\n"
                   L"  -m <挂载点>   盘符或目录 (默认为自动选择)\n"
                   L"  -i <毫秒>     内核元数据缓存超时 (默认值 1000)\n"
                   L"  -t <线程数>   分发线程数 (默认值 0)\n"
                   L"  -d <级别>     调试日志级别\n"
                   L"  -D <文件>     调试日志文件 (标准错误用 -)\n");
}

/* 程序主入口，解析参数、初始化 WinFsp 并启动文件系统 */
int wmain(int argc, wchar_t **argv) {
  _setmode(_fileno(stdout), _O_U16TEXT);
  _setmode(_fileno(stderr), _O_U16TEXT);

  PWSTR UpperArg = 0, LowerArg = 0, MountPoint = 0, DebugLogFile = 0;
  ULONG InfoTimeout = 1000, Threads = 0, DebugLog = 0;

  for (int i = 1; i < argc; i++) {
    if (wcscmp(argv[i], L"-h") == 0) {
      Usage();
      return 0;
    } else if (i + 1 >= argc) {
      Usage();
      return 2;
    } else if (wcscmp(argv[i], L"-u") == 0)
      UpperArg = argv[++i];
    else if (wcscmp(argv[i], L"-l") == 0)
      LowerArg = argv[++i];
    else if (wcscmp(argv[i], L"-m") == 0)
      MountPoint = argv[++i];
    else if (wcscmp(argv[i], L"-i") == 0)
      InfoTimeout = _wtoi(argv[++i]);
    else if (wcscmp(argv[i], L"-t") == 0)
      Threads = _wtoi(argv[++i]);
    else if (wcscmp(argv[i], L"-d") == 0)
      DebugLog = _wtoi(argv[++i]);
    else if (wcscmp(argv[i], L"-D") == 0)
      DebugLogFile = argv[++i];
    else {
      Usage();
      return 2;
    }
  }
  if (!UpperArg || !LowerArg) {
    Usage();
    return 2;
  }

  if (DebugLogFile) {
    HANDLE LogHandle = INVALID_HANDLE_VALUE;
    if (wcscmp(DebugLogFile, L"-") == 0)
      LogHandle = GetStdHandle(STD_ERROR_HANDLE);
    else
      LogHandle = CreateFileW(DebugLogFile, GENERIC_WRITE, FILE_SHARE_READ, 0,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);

    if (LogHandle != INVALID_HANDLE_VALUE && LogHandle != NULL) {
      FspDebugLogSetHandle(LogHandle);
    } else {
      fwprintf(stderr, L"无法打开日志文件: %s (错误码: %lu)\n", DebugLogFile,
               GetLastError());
      return 1;
    }
  }

  if (!NormalizeRootDir(UpperArg, g_UpperRoot, MAX_PATH)) {
    fwprintf(stderr, L"无效的顶层目录\n");
    return 1;
  }
  {
    WCHAR Probe[MAX_PATH];
    swprintf_s(Probe, MAX_PATH, L"%s\\%s_ovlfs_test", g_UpperRoot,
               WHITEOUT_PREFIX);
    HANDLE h = CreateFileW(Probe, GENERIC_WRITE, 0, 0, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY, 0);
    if (h == INVALID_HANDLE_VALUE) {
      fwprintf(stderr, L"顶层目录不存在或不可写: %s\n", g_UpperRoot);
      return 1;
    }
    CloseHandle(h);
    DeleteFileW(Probe);
  }

  if (!NormalizeRootDir(LowerArg, g_LowerRoot, MAX_PATH)) {
    fwprintf(stderr, L"无效的底层目录: %s\n", LowerArg);
    return 1;
  }
  if (GetFileAttributesW(g_LowerRoot) == INVALID_FILE_ATTRIBUTES) {
    fwprintf(stderr, L"底层目录不存在: %s\n", g_LowerRoot);
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
           sizeof Vp.FileSystemName / sizeof Vp.FileSystemName[0], g_Label);

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
  Iface.CanDelete = OvlCanDelete;
  Iface.Rename = OvlRename;
  Iface.ReadDirectory = OvlReadDirectory;
  Iface.GetDirInfoByName = OvlGetDirInfoByName;
  Iface.DispatcherStopped = OvlDispatcherStopped;

  NTSTATUS R = FspFileSystemPreflight(OVL_DISK_DEVICE_NAME, MountPoint);
  if (!NT_SUCCESS(R)) {
    fwprintf(stderr, L"Preflight 失败: %08lX\n", R);
    return 1;
  }
  FSP_FILE_SYSTEM *FileSystem;
  R = FspFileSystemCreate(OVL_DISK_DEVICE_NAME, &Vp, &Iface, &FileSystem);
  if (!NT_SUCCESS(R)) {
    fwprintf(stderr, L"Create 失败: %08lX\n", R);
    return 1;
  }
  if (DebugLog)
    FspFileSystemSetDebugLog(FileSystem, DebugLog);
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

  SetConsoleCtrlHandler(CtrlHandler, TRUE);
  wprintf(OVL_NAME L" 已挂载: %s (顶层: %s, 底层: %s)\n",
          FspFileSystemMountPoint(FileSystem), g_UpperRoot, g_LowerRoot);
  fflush(stdout);

  WaitForSingleObject(g_StopEvent, INFINITE);

  FspFileSystemStopDispatcher(FileSystem);
  FspFileSystemRemoveMountPoint(FileSystem);
  FspFileSystemDelete(FileSystem);
  wprintf(OVL_NAME L" 已卸载\n");
  return 0;
}
