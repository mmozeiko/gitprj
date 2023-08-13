#define WIN32_LEAN_AND_MEAN
#define _CRT_NONSTDC_NO_DEPRECATE
#include <windows.h>
#include <combaseapi.h>
#include <projectedfslib.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include "git2.h"

#pragma comment (lib, "synchronization")
#pragma comment (lib, "projectedfslib")
#pragma comment (lib, "advapi32")
#pragma comment (lib, "secur32")
#pragma comment (lib, "ole32")

#pragma comment (lib, "git2")

static int error_git(int err)
{
	const git_error *error = git_error_last();
	fprintf(stderr, "ERROR: %d, %s\n", err, error ? error->message : NULL);
	return EXIT_FAILURE;
}

static int error_hr(HRESULT hr)
{
	char msg[1024];
	FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, NULL, hr, 0, msg, sizeof(msg), NULL);
	fprintf(stderr, "ERROR: 0x%08x, %s\n", hr, msg);
	return EXIT_FAILURE;
}

typedef struct {
	bool isDir;
	size_t size;
	WCHAR* name;
} DirEntry;

typedef struct DirEnum {
	GUID guid;
	struct DirEnum* prev;
	struct DirEnum* next;

	WCHAR* search;

	size_t current;
	size_t count;
	DirEntry* entries;
} DirEnum;

typedef struct {
	git_repository* repo;
	DirEnum enums;
	volatile LONG enumLock;
} State;

static int DirEntrySort(const void* a, const void* b)
{
	const DirEntry* entryA = a;
	const DirEntry* entryB = b;
	return PrjFileNameCompare(entryA->name, entryB->name);
}

static LPSTR toUtf8(LPCWSTR wstr)
{
	if (!wstr) return NULL;
	int len = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
	char* str = malloc(len + 1);
	WideCharToMultiByte(CP_UTF8, 0, wstr, -1, str, len + 1, NULL, NULL);
	str[len] = 0;
	return str;
}

static LPWSTR fromUtf8(LPCSTR str)
{
	if (!str) return NULL;
	int len = MultiByteToWideChar(CP_UTF8, 0, str, -1, NULL, 0);
	wchar_t* wstr = calloc(len + 1, sizeof(*wstr));
	MultiByteToWideChar(CP_UTF8, 0, str, -1, wstr, len + 1);
	return wstr;
}

static DirEnum* CreateEnumFromTags(git_repository* repo)
{
	DirEntry* entries = NULL;
	size_t count = 0;

	git_strarray tags;
	if (git_tag_list(&tags, repo) == 0)
	{
		count = tags.count;

		entries = calloc(count, sizeof(DirEntry));
		for (size_t i = 0; i < count; i++)
		{
			DirEntry* dirEntry = entries + i;
			dirEntry->isDir = true;
			dirEntry->name = fromUtf8(tags.strings[i]);
		}

		git_strarray_free(&tags);
	}

	DirEnum* dirEnum = calloc(1, sizeof(*dirEnum));
	dirEnum->entries = entries;
	dirEnum->count = count;
	return dirEnum;
}

static DirEnum* CreateEnumFromTree(git_tree* tree)
{
	git_repository* repo = git_tree_owner(tree);

	const size_t chunk = 1024;

	DirEntry* entries = NULL;
	size_t capacity = 0;
	size_t count = 0;

	size_t entrycount = git_tree_entrycount(tree);
	for (size_t t = 0; t < entrycount; t++)
	{
		const git_tree_entry* entry = git_tree_entry_byindex(tree, t);
		if (entry)
		{
			git_object* child;
			if (git_tree_entry_to_object(&child, repo, entry) == 0)
			{
				git_object_t type = git_tree_entry_type(entry);

				if (type == GIT_OBJECT_BLOB || type == GIT_OBJECT_TREE)
				{
					if (count == capacity)
					{
						entries = realloc(entries, (capacity += chunk) * sizeof(DirEntry));
					}
					DirEntry* dirEntry = entries + count++;
					*dirEntry = (DirEntry){ 0 };

					if (type == GIT_OBJECT_BLOB)
					{
						git_object* obj;
						if (git_tree_entry_to_object(&obj, repo, entry) == 0)
						{
							dirEntry->size = git_blob_rawsize((git_blob*)obj);
							git_object_free(obj);
						}
					}
					else if (type == GIT_OBJECT_TREE)
					{
						dirEntry->isDir = TRUE;
					}
					dirEntry->name = fromUtf8(git_tree_entry_name(entry));
				}
			}
		}
	}

	DirEnum* dirEnum = calloc(1, sizeof(*dirEnum));
	dirEnum->entries = entries;
	dirEnum->count = count;
	return dirEnum;
}

static void StateLock(State* state)
{
	while (InterlockedCompareExchange(&state->enumLock, 1, 0) == 0)
	{
		LONG zero = 0;
		WaitOnAddress(&state->enumLock, &zero, sizeof(zero), INFINITE);
	}
}

static void StateUnlock(State* state)
{
	InterlockedExchange(&state->enumLock, 0);
}

// split path on first '\\'
static void SplitPath(LPCWSTR path, LPWSTR* first, LPWSTR* rest)
{
	wchar_t* slash = wcschr(path, L'\\');
	if (slash == NULL)
	{
		*first = wcsdup(path);
		*rest = NULL;
	}
	else
	{
		size_t flen = slash - path;
		*first = calloc(flen + 1, sizeof(WCHAR));
		memcpy(*first, path, flen * sizeof(WCHAR));
		*rest = wcsdup(slash + 1);
		for (WCHAR* s = *rest; *s; ++s)
		{
			if (*s == L'\\')
			{
				*s = L'/';
			}
		}
	}
}

// https://learn.microsoft.com/en-us/windows/win32/projfs/enumerating-files-and-directories
static HRESULT CALLBACK OnStartDirectoryEnumeration(const PRJ_CALLBACK_DATA* callbackData, const GUID* enumerationId)
{
	State* state = callbackData->InstanceContext;

	DirEnum* e = NULL;

	LPCWSTR relpath = callbackData->FilePathName;
	if (relpath[0] == 0)
	{
		e = CreateEnumFromTags(state->repo);
	}
	else
	{
		LPWSTR wtag;
		LPWSTR wpath;
		SplitPath(relpath, &wtag, &wpath);

		char* tag = toUtf8(wtag);
		char* path = toUtf8(wpath);

		int err;
		git_object* tagObj;
		err = git_revparse_single(&tagObj, state->repo, tag);
		if (err == 0)
		{
			if (git_tag_target_type((git_tag*)tagObj) == GIT_OBJECT_COMMIT)
			{
				git_commit* tagCommit;
				err = git_commit_lookup(&tagCommit, state->repo, git_tag_target_id((git_tag*)tagObj));
				if (err == 0)
				{
					git_tree* tagTree;
					err = git_commit_tree(&tagTree, tagCommit);
					if (err == 0)
					{
						if (path == NULL)
						{
							e = CreateEnumFromTree(tagTree);
						}
						else
						{
							git_object* dirObj;
							err = git_object_lookup_bypath(&dirObj, (git_object*)tagTree, path, GIT_OBJECT_TREE);
							if (err == 0)
							{
								e = CreateEnumFromTree((git_tree*)dirObj);
								git_object_free(dirObj);
							}
						}
						git_tree_free(tagTree);
					}
					git_commit_free(tagCommit);
				}
			}
			git_object_free(tagObj);
		}

		free(tag);
		free(path);

		free(wtag);
		free(wpath);
	}

	if (e)
	{
		qsort(e->entries, e->count, sizeof(DirEntry), &DirEntrySort);

		e->guid = *enumerationId;

		StateLock(state);
		e->next = state->enums.next;
		e->prev = &state->enums;
		e->next->prev = e;
		state->enums.next = e;
		StateUnlock(state);
	}

	return e ? S_OK : HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

static HRESULT CALLBACK OnEndDirectoryEnumeration(const PRJ_CALLBACK_DATA* callbackData, const GUID* enumerationId)
{
	State* state = callbackData->InstanceContext;

	DirEnum* e = NULL;

	StateLock(state);
	for (DirEnum* dirEnum = state->enums.next; dirEnum != &state->enums; dirEnum = dirEnum->next)
	{
		if (IsEqualGUID(&dirEnum->guid, enumerationId))
		{
			e = dirEnum;
			break;
		}
	}
	if (e)
	{
		e->next->prev = e->prev;
		e->prev->next = e->next;
	}
	StateUnlock(state);

	for (size_t i = 0; i < e->count; i++)
	{
		free(e->entries[i].name);
	}
	free(e->entries);
	free(e->search);
	free(e);

	return e ? S_OK : HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER);
}

static HRESULT CALLBACK OnGetDirectoryEnumeration(const PRJ_CALLBACK_DATA* callbackData, const GUID* enumerationId, PCWSTR searchExpression, PRJ_DIR_ENTRY_BUFFER_HANDLE dirEntryBufferHandle)
{
	State* state = callbackData->InstanceContext;

	DirEnum* e = NULL;

	StateLock(state);
	for (DirEnum* dirEnum = state->enums.next; dirEnum != &state->enums; dirEnum = dirEnum->next)
	{
		if (IsEqualGUID(&dirEnum->guid, enumerationId))
		{
			e = dirEnum;
			break;
		}
	}
	StateUnlock(state);

	if (!e)
	{
		return HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER);
	}

	if (e->search == NULL || (callbackData->Flags & PRJ_CB_DATA_FLAG_ENUM_RESTART_SCAN))
	{
		free(e->search);
		e->search = wcsdup(searchExpression);
		e->current = 0;
	}

	while (e->current != e->count)
	{
		DirEntry* entry = e->entries + e->current;
		if (PrjFileNameMatch(entry->name, e->search))
		{
			PRJ_FILE_BASIC_INFO info =
			{
				.IsDirectory = entry->isDir,
				.FileSize = entry->size,
				.FileAttributes = entry->isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_READONLY,
			};

			HRESULT hr = PrjFillDirEntryBuffer(entry->name, &info, dirEntryBufferHandle);
			if (hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER))
			{
				return S_OK;
			}
			else if (FAILED(hr))
			{
				return hr;
			}
		}

		e->current += 1;
	}

	return S_OK;
}

// https://learn.microsoft.com/en-us/windows/win32/projfs/providing-file-data#placeholder-creation
static HRESULT CALLBACK OnGetPlaceholderInfo(const PRJ_CALLBACK_DATA* callbackData)
{
	State* state = callbackData->InstanceContext;

	LPCWSTR relpath = callbackData->FilePathName;

	LPWSTR wtag;
	LPWSTR wpath;
	SplitPath(relpath, &wtag, &wpath);

	char* tag = toUtf8(wtag);
	char* path = toUtf8(wpath);

	HRESULT hr = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
	if (path == NULL)
	{
		// check if tag exists
		git_strarray tags;
		if (git_tag_list(&tags, state->repo) == 0)
		{
			for (size_t i = 0; i < tags.count; i++)
			{
				LPWSTR tagname = fromUtf8(tags.strings[i]);

				if (PrjFileNameCompare(tagname, wtag) == 0)
				{
					free(tagname);

					PRJ_PLACEHOLDER_INFO info =
					{
						.FileBasicInfo =
						{
							.IsDirectory = TRUE,
							.FileAttributes = FILE_ATTRIBUTE_DIRECTORY,
						},
					};

					hr = PrjWritePlaceholderInfo(callbackData->NamespaceVirtualizationContext, relpath, &info, sizeof(info));
					break;
				}
				free(tagname);
			}

			git_strarray_free(&tags);
		}
	}
	else
	{
		// check if path exists in tag
		int err;
		git_object* tagObj;
		err = git_revparse_single(&tagObj, state->repo, tag);
		if (err == 0)
		{
			if (git_tag_target_type((git_tag*)tagObj) == GIT_OBJECT_COMMIT)
			{
				git_commit* tagCommit;
				err = git_commit_lookup(&tagCommit, state->repo, git_tag_target_id((git_tag*)tagObj));
				if (err == 0)
				{
					git_tree* tagTree;
					err = git_commit_tree(&tagTree, tagCommit);
					if (err == 0)
					{
						git_tree_entry* entry;
						err = git_tree_entry_bypath(&entry, tagTree, path);
						if (err == 0)
						{
							git_object* obj;
							err = git_tree_entry_to_object(&obj, state->repo, entry);
							if (err == 0)
							{
								if (git_object_type(obj) == GIT_OBJECT_BLOB)
								{
									PRJ_PLACEHOLDER_INFO info =
									{
										.FileBasicInfo =
										{
											.IsDirectory = FALSE,
											.FileSize = git_blob_rawsize((git_blob*)obj),
											.FileAttributes = FILE_ATTRIBUTE_READONLY,
										},
									};
									hr = PrjWritePlaceholderInfo(callbackData->NamespaceVirtualizationContext, relpath, &info, sizeof(info));
								}
								else if (git_object_type(obj) == GIT_OBJECT_TREE)
								{
									PRJ_PLACEHOLDER_INFO info =
									{
										.FileBasicInfo =
										{
											.IsDirectory = TRUE,
											.FileAttributes = FILE_ATTRIBUTE_DIRECTORY,
										},
									};
									hr = PrjWritePlaceholderInfo(callbackData->NamespaceVirtualizationContext, relpath, &info, sizeof(info));
								}
								git_object_free(obj);
							}
							git_tree_entry_free(entry);
						}
						git_tree_free(tagTree);
					}
					git_commit_free(tagCommit);
				}
			}
			git_object_free(tagObj);
		}
	}

	free(tag);
	free(path);
	free(wtag);
	free(wpath);

	return hr;
}

//static HRESULT CALLBACK OnQueryFileName(const PRJ_CALLBACK_DATA* callbackData)
//{
//	State* state = callbackData->InstanceContext;
//
//	LPCWSTR relpath = callbackData->FilePathName;
//
//	LPWSTR wtag;
//	LPWSTR wpath;
//	SplitPath(relpath, &wtag, &wpath);
//
//	char* tag = toUtf8(wtag);
//	char* path = toUtf8(wpath);
//
//	HRESULT hr = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
//	if (path == NULL)
//	{
//		// check if tag exists
//		git_strarray tags;
//		if (git_tag_list(&tags, state->repo) == 0)
//		{
//			for (size_t i = 0; i < tags.count; i++)
//			{
//				LPWSTR tagname = fromUtf8(tags.strings[i]);
//				if (PrjFileNameCompare(tagname, wtag) == 0)
//				{
//					free(tagname);
//					hr = S_OK;
//					break;
//				}
//				free(tagname);
//			}
//
//			git_strarray_free(&tags);
//		}
//	}
//	else
//	{
//		// check if path exists in tag
//		int err;
//		git_object* tagObj;
//		err = git_revparse_single(&tagObj, state->repo, tag);
//		if (err == 0)
//		{
//			if (git_tag_target_type((git_tag*)tagObj) == GIT_OBJECT_COMMIT)
//			{
//				git_commit* tagCommit;
//				err = git_commit_lookup(&tagCommit, state->repo, git_tag_target_id((git_tag*)tagObj));
//				if (err == 0)
//				{
//					git_tree* tagTree;
//					err = git_commit_tree(&tagTree, tagCommit);
//					if (err == 0)
//					{
//						git_tree_entry* entry;
//						err = git_tree_entry_bypath(&entry, tagTree, path);
//						if (err == 0)
//						{
//							hr = S_OK;
//							git_tree_entry_free(entry);
//						}
//						git_tree_free(tagTree);
//					}
//					git_commit_free(tagCommit);
//				}
//			}
//			git_object_free(tagObj);
//		}
//	}
//
//	free(tag);
//	free(path);
//	free(wtag);
//	free(wpath);
//
//	return hr;
//}


// https://learn.microsoft.com/en-us/windows/win32/projfs/providing-file-data#providing-file-contents
static HRESULT CALLBACK OnGetFileData(const PRJ_CALLBACK_DATA* callbackData, UINT64 byteOffset, UINT32 length)
{
	State* state = callbackData->InstanceContext;

	LPCWSTR relpath = callbackData->FilePathName;

	LPWSTR wtag;
	LPWSTR wpath;
	SplitPath(relpath, &wtag, &wpath);

	char* tag = toUtf8(wtag);
	char* path = toUtf8(wpath);

	HRESULT hr = E_FAIL;

	int err;
	git_object* tagObj;
	err = git_revparse_single(&tagObj, state->repo, tag);
	if (err == 0)
	{
		if (git_tag_target_type((git_tag*)tagObj) == GIT_OBJECT_COMMIT)
		{
			git_commit* tagCommit;
			err = git_commit_lookup(&tagCommit, state->repo, git_tag_target_id((git_tag*)tagObj));
			if (err == 0)
			{
				git_tree* tagTree;
				err = git_commit_tree(&tagTree, tagCommit);
				if (err == 0)
				{
					git_object* obj;
					err = git_object_lookup_bypath(&obj, (git_object*)tagTree, path, GIT_OBJECT_BLOB);
					if (err == 0)
					{
						void* dst = PrjAllocateAlignedBuffer(callbackData->NamespaceVirtualizationContext, length);
						if (dst == NULL)
						{
							hr = E_OUTOFMEMORY;
						}
						else
						{
							const void* src = git_blob_rawcontent((git_blob*)obj);
							memcpy(dst, src, length);
							hr = PrjWriteFileData(callbackData->NamespaceVirtualizationContext, &callbackData->DataStreamId, dst, byteOffset, length);
							PrjFreeAlignedBuffer(dst);
						}

						git_object_free(obj);
					}
					git_tree_free(tagTree);
				}
				git_commit_free(tagCommit);
			}
			git_object_free(tagObj);
		}
	}

	return hr;
}

// https://learn.microsoft.com/en-us/windows/win32/projfs/file-system-operation-notifications
static HRESULT CALLBACK OnNotification(const PRJ_CALLBACK_DATA* callbackData, BOOLEAN isDirectory, PRJ_NOTIFICATION notification, PCWSTR destinationFileName, PRJ_NOTIFICATION_PARAMETERS* operationParameters)
{
	State* state = callbackData->InstanceContext;
	switch (notification)
	{
	case PRJ_NOTIFICATION_FILE_OPENED: // after existing file has been opened
	case PRJ_NOTIFICATION_FILE_HANDLE_CLOSED_NO_MODIFICATION:
		return S_OK;
	case PRJ_NOTIFICATION_NEW_FILE_CREATED: // after new file created
	case PRJ_NOTIFICATION_FILE_OVERWRITTEN: // after file overwritten
	case PRJ_NOTIFICATION_PRE_DELETE: // before file deleted
	case PRJ_NOTIFICATION_PRE_RENAME: // before file renamed
	case PRJ_NOTIFICATION_PRE_SET_HARDLINK: // before hardlink created
	case PRJ_NOTIFICATION_FILE_RENAMED: // after file renamed
	case PRJ_NOTIFICATION_HARDLINK_CREATED: // after hardlink created
	case PRJ_NOTIFICATION_FILE_HANDLE_CLOSED_FILE_MODIFIED:
	case PRJ_NOTIFICATION_FILE_HANDLE_CLOSED_FILE_DELETED:
	case PRJ_NOTIFICATION_FILE_PRE_CONVERT_TO_FULL: // placeholder -> full file, before modified
		return E_FAIL;
	}
	return E_NOTIMPL;
}

int main(int argc, char* argv[])
{
	if (argc != 2)
	{
		fprintf(stderr, "USAGE: %s PATH_TO_REPO\n", argv[0]);
		return EXIT_FAILURE;
	}

	git_libgit2_init();

	State state = { 0 };
	state.enums.next = state.enums.prev = &state.enums;

	int err = git_repository_open(&state.repo, argv[1]);
	if (err != 0) return error_git(err);

	PRJ_STARTVIRTUALIZING_OPTIONS opts =
	{
		.NotificationMappings = &(PRJ_NOTIFICATION_MAPPING)
		{
			.NotificationRoot = L"",
			.NotificationBitMask =
				PRJ_NOTIFY_NEW_FILE_CREATED |
				PRJ_NOTIFY_FILE_OVERWRITTEN |
				PRJ_NOTIFY_PRE_DELETE |
				PRJ_NOTIFY_PRE_RENAME |
				PRJ_NOTIFY_PRE_SET_HARDLINK |
				PRJ_NOTIFY_FILE_PRE_CONVERT_TO_FULL,
		},
		.NotificationMappingsCount = 1,
	};

	PRJ_CALLBACKS callbacks =
	{
		.StartDirectoryEnumerationCallback = &OnStartDirectoryEnumeration,
		.EndDirectoryEnumerationCallback = &OnEndDirectoryEnumeration,
		.GetDirectoryEnumerationCallback = &OnGetDirectoryEnumeration,
		.GetPlaceholderInfoCallback = &OnGetPlaceholderInfo,
		.GetFileDataCallback = &OnGetFileData,
		//.QueryFileNameCallback = &OnQueryFileName,
		.NotificationCallback = &OnNotification,
		//.CancelCommandCallback,
	};

	HRESULT hr;
	
	GUID guid;
	CoCreateGuid(&guid);

	CreateDirectoryW(L"tags", NULL);
	PrjMarkDirectoryAsPlaceholder(L"tags", NULL, NULL, &guid);

	PRJ_NAMESPACE_VIRTUALIZATION_CONTEXT context;
	hr = PrjStartVirtualizing(L"tags", &callbacks, &state, &opts, &context);
	if (FAILED(hr)) return error_hr(hr);

	printf("Press ENTER to stop!\n");
	char tmp;
	gets_s(&tmp, 1);

	PrjStopVirtualizing(context);

	return EXIT_SUCCESS;
}
