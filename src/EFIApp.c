#include "EFIApp.h"
#include "application.h"

#ifdef _ARM
#include <armintr.h>
#endif

// ====== Minimal PE/COFF structures ======

#pragma pack(push, 1)

typedef struct {
	UINT16 e_magic;
	UINT16 e_pad[29];
	UINT32 e_lfanew;
} IMAGE_DOS_HEADER;

typedef struct {
	UINT16 Machine;
	UINT16 NumberOfSections;
	UINT32 TimeDateStamp;
	UINT32 PointerToSymbolTable;
	UINT32 NumberOfSymbols;
	UINT16 SizeOfOptionalHeader;
	UINT16 Characteristics;
} IMAGE_FILE_HEADER;

typedef struct { UINT32 VirtualAddress; UINT32 Size; } IMAGE_DATA_DIRECTORY;

typedef struct {
	UINT16 Magic;
	UINT8  MajorLinkerVersion;
	UINT8  MinorLinkerVersion;
	UINT32 SizeOfCode;
	UINT32 SizeOfInitializedData;
	UINT32 SizeOfUninitializedData;
	UINT32 AddressOfEntryPoint;
	UINT32 BaseOfCode;
	UINT32 BaseOfData;
	UINT32 ImageBase;
	UINT32 SectionAlignment;
	UINT32 FileAlignment;
	UINT16 MajorOperatingSystemVersion;
	UINT16 MinorOperatingSystemVersion;
	UINT16 MajorImageVersion;
	UINT16 MinorImageVersion;
	UINT16 MajorSubsystemVersion;
	UINT16 MinorSubsystemVersion;
	UINT32 Win32VersionValue;
	UINT32 SizeOfImage;
	UINT32 SizeOfHeaders;
	UINT32 CheckSum;
	UINT16 Subsystem;
	UINT16 DllCharacteristics;
	UINT32 SizeOfStackReserve;
	UINT32 SizeOfStackCommit;
	UINT32 SizeOfHeapReserve;
	UINT32 SizeOfHeapCommit;
	UINT32 LoaderFlags;
	UINT32 NumberOfRvaAndSizes;
	IMAGE_DATA_DIRECTORY DataDirectory[16];
} IMAGE_OPTIONAL_HEADER32;

typedef struct {
	CHAR8  Name[8];
	UINT32 VirtualSize;
	UINT32 VirtualAddress;
	UINT32 SizeOfRawData;
	UINT32 PointerToRawData;
	UINT32 PointerToRelocations;
	UINT32 PointerToLineNumbers;
	UINT16 NumberOfRelocations;
	UINT16 NumberOfLineNumbers;
	UINT32 Characteristics;
} IMAGE_SECTION_HEADER;

typedef struct {
	UINT32 VirtualAddress;
	UINT32 SizeOfBlock;
} IMAGE_BASE_RELOCATION;

#pragma pack(pop)

#define IMAGE_DOS_SIGNATURE       0x5A4D
#define IMAGE_NT_SIGNATURE        0x00004550
#define IMAGE_DIRECTORY_ENTRY_BASERELOC 5

#define IMAGE_REL_BASED_ABSOLUTE   0
#define IMAGE_REL_BASED_HIGHLOW    3
#define IMAGE_REL_BASED_ARM_MOV32T 5

typedef EFI_STATUS (EFIAPI *EFI_IMAGE_ENTRY_POINT)(EFI_HANDLE, EFI_SYSTEM_TABLE*);

// ====== Tracking table for our manually-loaded images ======
#define MAX_TRACKED 16
typedef struct {
	EFI_HANDLE Handle;
	EFI_IMAGE_ENTRY_POINT Entry;
	VOID *ImageBase;
	UINTN ImageSize;
} TrackedImage;
static TrackedImage gTracked[MAX_TRACKED];
static UINTN gTrackedCount = 0;

// Saved original function pointers
static EFI_IMAGE_LOAD  gOrigLoadImage  = NULL;
static EFI_IMAGE_START gOrigStartImage = NULL;
static EFI_GET_VARIABLE gOrigGetVariable = NULL;

static EFI_STATUS EFIAPI HookedGetVariable(
	CHAR16 *VariableName,
	EFI_GUID *VendorGuid,
	UINT32 *Attributes,
	UINTN *DataSize,
	VOID *Data)
{
	// Spoof SecureBoot variable as "off" so kernel accepts dtb=cmdline
	if (VariableName && VariableName[0] == L'S' && VariableName[1] == L'e'
		&& VariableName[2] == L'c' && VariableName[3] == L'u'
		&& VariableName[4] == L'r' && VariableName[5] == L'e'
		&& VariableName[6] == L'B' && VariableName[7] == L'o'
		&& VariableName[8] == L'o' && VariableName[9] == L't'
		&& VariableName[10] == 0)
	{
		if (Data == NULL || *DataSize < 1) {
			*DataSize = 1;
			return EFI_BUFFER_TOO_SMALL;
		}
		*((UINT8*)Data) = 0;  // Secure Boot OFF
		*DataSize = 1;
		if (Attributes) *Attributes = EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;
		return EFI_SUCCESS;
	}
	return gOrigGetVariable(VariableName, VendorGuid, Attributes, DataSize, Data);
}

// ====== ARM Thumb-2 MOV32T relocation ======

static UINT32 Thumb2GetImm16(UINT16 a, UINT16 b)
{
	return ((a & 0xF) << 12) | (((a >> 10) & 1) << 11) | (((b >> 12) & 7) << 8) | (b & 0xFF);
}

static void Thumb2SetImm16(UINT16 *inst, UINT32 imm16)
{
	UINT32 imm8 = imm16 & 0xFF;
	UINT32 imm3 = (imm16 >> 8) & 0x7;
	UINT32 i    = (imm16 >> 11) & 0x1;
	UINT32 imm4 = (imm16 >> 12) & 0xF;
	inst[0] = (UINT16)((inst[0] & ~0x040F) | imm4 | (i << 10));
	inst[1] = (UINT16)((inst[1] & ~0x70FF) | imm8 | (imm3 << 12));
}

static void RelocateArmMov32T(UINT8 *Patch, INTN Delta)
{
	UINT16 *movw = (UINT16*)Patch;
	UINT16 *movt = (UINT16*)(Patch + 4);
	UINT32 lo = Thumb2GetImm16(movw[0], movw[1]);
	UINT32 hi = Thumb2GetImm16(movt[0], movt[1]);
	UINT32 val = (hi << 16) | lo;
	val = (UINT32)((INTN)val + Delta);
	Thumb2SetImm16(movw, val & 0xFFFF);
	Thumb2SetImm16(movt, (val >> 16) & 0xFFFF);
}

// ====== Manual PE loader: load only (no entry call) ======

static EFI_STATUS ManualLoad(
	VOID *FileBuffer,
	UINTN FileSize,
	EFI_HANDLE DeviceHandle,
	EFI_HANDLE ParentImageHandle,
	VOID *LoadOptions,
	UINT32 LoadOptionsSize,
	EFI_HANDLE *OutHandle)
{
	EFI_STATUS Status;
	IMAGE_DOS_HEADER *Dos = (IMAGE_DOS_HEADER*)FileBuffer;
	if (Dos->e_magic != IMAGE_DOS_SIGNATURE) return EFI_LOAD_ERROR;

	UINT8 *PeBase = (UINT8*)FileBuffer + Dos->e_lfanew;
	if (*(UINT32*)PeBase != IMAGE_NT_SIGNATURE) return EFI_LOAD_ERROR;

	IMAGE_FILE_HEADER *FileHdr = (IMAGE_FILE_HEADER*)(PeBase + 4);
	IMAGE_OPTIONAL_HEADER32 *Opt = (IMAGE_OPTIONAL_HEADER32*)((UINT8*)FileHdr + sizeof(IMAGE_FILE_HEADER));
	IMAGE_SECTION_HEADER *Sections = (IMAGE_SECTION_HEADER*)((UINT8*)Opt + FileHdr->SizeOfOptionalHeader);

	UINT32 SizeOfImage = Opt->SizeOfImage;
	UINT32 EntryRVA    = Opt->AddressOfEntryPoint;
	UINT32 Preferred   = Opt->ImageBase;

	EFI_PHYSICAL_ADDRESS LoadAddr = 0;
	UINTN NumPages = (SizeOfImage + 0xFFF) >> 12;
	Status = gBS->AllocatePages(AllocateAnyPages, EfiLoaderCode, NumPages, &LoadAddr);
	if (EFI_ERROR(Status)) return Status;

	UINT8 *Image = (UINT8*)(UINTN)LoadAddr;
	gBS->SetMem(Image, SizeOfImage, 0);
	gBS->CopyMem(Image, FileBuffer, Opt->SizeOfHeaders);

	for (UINT16 i = 0; i < FileHdr->NumberOfSections; i++) {
		IMAGE_SECTION_HEADER *S = &Sections[i];
		if (S->SizeOfRawData)
			gBS->CopyMem(Image + S->VirtualAddress, (UINT8*)FileBuffer + S->PointerToRawData, S->SizeOfRawData);
	}

	Print(L"[Load] machine=0x%x SizeOfImage=%d EntryRVA=0x%x PrefBase=0x%x LoadAt=0x%x\n",
		FileHdr->Machine, SizeOfImage, EntryRVA, Preferred, (UINT32)(UINTN)Image);

	INTN Delta = (INTN)(UINTN)Image - (INTN)Preferred;
	UINT32 RelocCounts[16] = {0};
	UINT32 RelocApplied = 0;
	if (Delta != 0 && Opt->NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_BASERELOC
		&& Opt->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size > 0)
	{
		UINT32 RV = Opt->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].VirtualAddress;
		UINT32 RS = Opt->DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size;
		IMAGE_BASE_RELOCATION *B = (IMAGE_BASE_RELOCATION*)(Image + RV);
		UINT8 *End = (UINT8*)B + RS;
		while ((UINT8*)B < End && B->SizeOfBlock > sizeof(IMAGE_BASE_RELOCATION)) {
			UINT32 N = (B->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(UINT16);
			UINT16 *E = (UINT16*)((UINT8*)B + sizeof(IMAGE_BASE_RELOCATION));
			UINT8 *Page = Image + B->VirtualAddress;
			for (UINT32 j = 0; j < N; j++) {
				UINT16 e = E[j];
				UINT16 t = e >> 12;
				UINT16 o = e & 0xFFF;
				if (t < 16) RelocCounts[t]++;
				if (t == IMAGE_REL_BASED_HIGHLOW) { UINT32 *T = (UINT32*)(Page + o); *T = (UINT32)((INTN)*T + Delta); RelocApplied++; }
				else if (t == IMAGE_REL_BASED_ARM_MOV32T) { RelocateArmMov32T(Page + o, Delta); RelocApplied++; }
			}
			B = (IMAGE_BASE_RELOCATION*)((UINT8*)B + B->SizeOfBlock);
		}
		Print(L"[Load] reloc: applied=%d types: ", RelocApplied);
		for (int i = 0; i < 16; i++) if (RelocCounts[i] > 0) Print(L"t%d=%d ", i, RelocCounts[i]);
		Print(L"\n");
	}

	EFI_LOADED_IMAGE *Li = NULL;
	Status = gBS->AllocatePool(EfiLoaderData, sizeof(EFI_LOADED_IMAGE), (VOID**)&Li);
	if (EFI_ERROR(Status)) return Status;
	gBS->SetMem(Li, sizeof(EFI_LOADED_IMAGE), 0);
	Li->Revision = EFI_LOADED_IMAGE_PROTOCOL_REVISION;
	Li->ParentHandle = ParentImageHandle;
	Li->SystemTable = gST;
	Li->DeviceHandle = DeviceHandle;
	Li->FilePath = NULL;
	Li->LoadOptions = LoadOptions;
	Li->LoadOptionsSize = LoadOptionsSize;
	Li->ImageBase = (VOID*)Image;
	Li->ImageSize = SizeOfImage;
	Li->ImageCodeType = EfiLoaderCode;
	Li->ImageDataType = EfiLoaderData;
	Li->Unload = NULL;

	EFI_HANDLE NewHandle = NULL;
	Status = gBS->InstallProtocolInterface(&NewHandle, &gEfiLoadedImageProtocolGuid, EFI_NATIVE_INTERFACE, Li);
	if (EFI_ERROR(Status)) return Status;

	if (gTrackedCount < MAX_TRACKED) {
		// EntryRVA's bit 0 encodes mode: 1=Thumb, 0=ARM.
		// Trust the file - do NOT force Thumb bit.
		UINTN EntryAddr = (UINTN)Image + EntryRVA;
		Print(L"[Load] EntryAddr=0x%x mode=%s\n",
			(UINT32)EntryAddr, (EntryAddr & 1) ? L"Thumb" : L"ARM");
		gTracked[gTrackedCount].Handle = NewHandle;
		gTracked[gTrackedCount].Entry = (EFI_IMAGE_ENTRY_POINT)EntryAddr;
		gTracked[gTrackedCount].ImageBase = Image;
		gTracked[gTrackedCount].ImageSize = SizeOfImage;
		gTrackedCount++;
	}

#ifdef _ARM
	// Cache coherency: clean D-cache by MVA over the loaded image, then
	// invalidate I-cache + branch predictor. Without the D-cache clean,
	// kernel code we just wrote is still in D-cache and the I-cache refills
	// from memory get stale data — CPU executes garbage on first kernel insn.
	{
		UINT8 *p = (UINT8*)Image;
		UINT8 *end = p + SizeOfImage;
		// Cortex-A9 line size = 32 bytes
		while (p < end) {
			_MoveToCoprocessor((UINT32)(UINTN)p, 15, 0, 7, 10, 1);  // DCCMVAC
			p += 32;
		}
	}
	__dsb(_ARM_BARRIER_SY);
	_MoveToCoprocessor(0, 15, 0, 7, 5, 0);  // ICIALLU
	_MoveToCoprocessor(0, 15, 0, 7, 5, 6);  // BPIALL
	__dsb(_ARM_BARRIER_SY);
	__isb(_ARM_BARRIER_SY);
#endif

	*OutHandle = NewHandle;
	return EFI_SUCCESS;
}

// ====== Hooked LoadImage ======

static EFI_STATUS EFIAPI HookedLoadImage(
	BOOLEAN BootPolicy,
	EFI_HANDLE ParentImageHandle,
	EFI_DEVICE_PATH *FilePath,
	VOID *SourceBuffer,
	UINTN SourceSize,
	EFI_HANDLE *ImageHandle)
{
	Print(L"[Hook] LoadImage called (buf=%d, size=%d)\n", SourceBuffer != NULL, SourceSize);
	// Try original first
	EFI_STATUS Status = gOrigLoadImage(BootPolicy, ParentImageHandle, FilePath, SourceBuffer, SourceSize, ImageHandle);
	Print(L"[Hook] Orig LoadImage returned: %r\n", Status);
	if (Status != EFI_SECURITY_VIOLATION) return Status;
	Print(L"[Hook] Falling back to ManualLoad\n");

	// Security violation. Fall back to manual load.
	VOID *Buffer = SourceBuffer;
	UINTN Size = SourceSize;
	BOOLEAN FreeBuffer = FALSE;
	EFI_HANDLE DeviceHandle = NULL;

	// Get device handle for the parent so child knows where it came from
	EFI_LOADED_IMAGE *ParentLi = NULL;
	if (gBS->HandleProtocol(ParentImageHandle, &gEfiLoadedImageProtocolGuid, (VOID**)&ParentLi) == EFI_SUCCESS)
		DeviceHandle = ParentLi->DeviceHandle;

	// If no SourceBuffer, read file via FilePath
	if (Buffer == NULL && FilePath != NULL) {
		// Locate device handle and file path
		EFI_HANDLE Dev = NULL;
		EFI_DEVICE_PATH *RemainingPath = FilePath;
		if (gBS->LocateDevicePath(&gEfiSimpleFileSystemProtocolGuid, &RemainingPath, &Dev) != EFI_SUCCESS)
			return EFI_LOAD_ERROR;

		EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs = NULL;
		if (gBS->HandleProtocol(Dev, &gEfiSimpleFileSystemProtocolGuid, (VOID**)&Fs) != EFI_SUCCESS)
			return EFI_LOAD_ERROR;

		EFI_FILE_PROTOCOL *Root = NULL;
		if (Fs->OpenVolume(Fs, &Root) != EFI_SUCCESS) return EFI_LOAD_ERROR;

		// Build a wide-char path from RemainingPath (FILEPATH_DEVICE_PATH nodes)
		// Simplest: walk RemainingPath nodes of type MEDIA_DEVICE_PATH/MEDIA_FILEPATH_DP and concatenate.
		CHAR16 PathBuf[256];
		PathBuf[0] = 0;
		EFI_DEVICE_PATH *Node = RemainingPath;
		while (Node && !IsDevicePathEnd(Node)) {
			if (DevicePathType(Node) == MEDIA_DEVICE_PATH && DevicePathSubType(Node) == MEDIA_FILEPATH_DP) {
				CHAR16 *NodePath = (CHAR16*)((UINT8*)Node + 4);
				UINTN len = 0;
				while (PathBuf[len]) len++;
				UINTN i = 0;
				while (NodePath[i] && len + i < 255) { PathBuf[len + i] = NodePath[i]; i++; }
				PathBuf[len + i] = 0;
			}
			Node = NextDevicePathNode(Node);
		}

		EFI_FILE_PROTOCOL *F = NULL;
		if (Root->Open(Root, &F, PathBuf, EFI_FILE_MODE_READ, 0) != EFI_SUCCESS) {
			Root->Close(Root);
			return EFI_LOAD_ERROR;
		}

		EFI_FILE_INFO *Info = NULL;
		UINTN InfoSize = 0;
		F->GetInfo(F, &gEfiFileInfoGuid, &InfoSize, NULL);
		gBS->AllocatePool(EfiLoaderData, InfoSize, (VOID**)&Info);
		F->GetInfo(F, &gEfiFileInfoGuid, &InfoSize, Info);
		Size = (UINTN)Info->FileSize;
		gBS->FreePool(Info);

		if (gBS->AllocatePool(EfiLoaderData, Size, &Buffer) != EFI_SUCCESS) {
			F->Close(F); Root->Close(Root);
			return EFI_LOAD_ERROR;
		}
		FreeBuffer = TRUE;
		UINTN Read = Size;
		F->Read(F, &Read, Buffer);
		F->Close(F);
		Root->Close(Root);
		DeviceHandle = Dev;
	}

	Status = ManualLoad(Buffer, Size, DeviceHandle, ParentImageHandle, NULL, 0, ImageHandle);
	Print(L"[Hook] ManualLoad returned: %r, handle=0x%x\n", Status, (UINT32)(UINTN)*ImageHandle);
	if (FreeBuffer) gBS->FreePool(Buffer);
	return Status;
}

// ====== Hooked StartImage ======

static EFI_STATUS EFIAPI HookedStartImage(
	EFI_HANDLE ImageHandle,
	UINTN *ExitDataSize,
	CHAR16 **ExitData)
{
	Print(L"[Hook] StartImage(0x%x)\n", (UINT32)(UINTN)ImageHandle);
	for (UINTN i = 0; i < gTrackedCount; i++) {
		if (gTracked[i].Handle == ImageHandle) {
			// Inspect LoadOptions before calling
			EFI_LOADED_IMAGE *Li = NULL;
			if (gBS->HandleProtocol(ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID**)&Li) == EFI_SUCCESS) {
				Print(L"[Hook] LoadOptions=0x%x size=%d\n", (UINT32)(UINTN)Li->LoadOptions, Li->LoadOptionsSize);
				if (Li->LoadOptions && Li->LoadOptionsSize > 0 && Li->LoadOptionsSize < 512) {
					Print(L"[Hook] args: %s\n", (CHAR16*)Li->LoadOptions);
				}
			}
			Print(L"[Hook] -> calling entry at 0x%x\n", (UINT32)(UINTN)gTracked[i].Entry);
			EFI_STATUS Status = gTracked[i].Entry(ImageHandle, gST);
			Print(L"[Hook] <- entry returned: %r\n", Status);
			if (ExitDataSize) *ExitDataSize = 0;
			if (ExitData) *ExitData = NULL;
			return Status;
		}
	}
	Print(L"[Hook] handle not tracked, calling orig\n");
	return gOrigStartImage(ImageHandle, ExitDataSize, ExitData);
}

// ====== Main ======

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
	EFI_STATUS Status = EFI_SUCCESS;
	UINTN NumHandles = 0;
	EFI_HANDLE *Handles;
	EFI_SIMPLE_TEXT_OUT_PROTOCOL *ScreenOut;

	InitializeLib(ImageHandle, SystemTable);

	// Tegra 3 screen fix
	Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiSimpleTextOutProtocolGuid, NULL, &NumHandles, &Handles);
	if (!EFI_ERROR(Status) && NumHandles >= 3) {
		Status = gBS->HandleProtocol(Handles[NumHandles - 1], &gEfiSimpleTextOutProtocolGuid, (VOID**)&ScreenOut);
		if (!EFI_ERROR(Status)) {
			gST->ConOut = ScreenOut;
			gST->ConsoleOutHandle = Handles[NumHandles - 1];
		}
	}

	Print(L"BootShim: context switch complete\n");

	// Install LoadImage and StartImage hooks
	gOrigLoadImage  = gBS->LoadImage;
	gOrigStartImage = gBS->StartImage;
	gBS->LoadImage  = HookedLoadImage;
	gBS->StartImage = HookedStartImage;
	gBS->Hdr.CRC32 = 0;
	UINT32 NewCrc = 0;
	gBS->CalculateCrc32((VOID*)gBS, gBS->Hdr.HeaderSize, &NewCrc);
	gBS->Hdr.CRC32 = NewCrc;

	// Hook GetVariable to spoof SecureBoot=0 (kernel rejects dtb=cmdline otherwise)
	gOrigGetVariable = gRT->GetVariable;
	gRT->GetVariable = HookedGetVariable;
	gRT->Hdr.CRC32 = 0;
	gBS->CalculateCrc32((VOID*)gRT, gRT->Hdr.HeaderSize, &NewCrc);
	gRT->Hdr.CRC32 = NewCrc;

	Print(L"BootShim: hooks installed (LoadImage/StartImage/GetVariable)\n");

	// Locate own image
	EFI_LOADED_IMAGE *Self = NULL;
	Status = gBS->HandleProtocol(ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID**)&Self);
	if (EFI_ERROR(Status)) { Print(L"LI failed: %r\n", Status); goto bail; }

	EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *Fs = NULL;
	Status = gBS->HandleProtocol(Self->DeviceHandle, &gEfiSimpleFileSystemProtocolGuid, (VOID**)&Fs);
	if (EFI_ERROR(Status)) { Print(L"FS failed: %r\n", Status); goto bail; }

	EFI_FILE_PROTOCOL *Root = NULL;
	Status = Fs->OpenVolume(Fs, &Root);
	if (EFI_ERROR(Status)) { Print(L"Vol failed: %r\n", Status); goto bail; }

	EFI_FILE_PROTOCOL *File = NULL;
	Status = Root->Open(Root, &File, L"\\efi\\boot\\shell.efi", EFI_FILE_MODE_READ, 0);
	if (EFI_ERROR(Status)) { Print(L"Open(shell) failed: %r\n", Status); goto bail; }

	EFI_FILE_INFO *Info = NULL;
	UINTN InfoSize = 0;
	File->GetInfo(File, &gEfiFileInfoGuid, &InfoSize, NULL);
	gBS->AllocatePool(EfiLoaderData, InfoSize, (VOID**)&Info);
	File->GetInfo(File, &gEfiFileInfoGuid, &InfoSize, Info);
	UINTN FileSize = (UINTN)Info->FileSize;
	Print(L"BootShim: shell.efi = %d bytes\n", FileSize);

	VOID *FileBuf = NULL;
	gBS->AllocatePool(EfiLoaderData, FileSize, &FileBuf);
	UINTN ReadSize = FileSize;
	File->Read(File, &ReadSize, FileBuf);
	File->Close(File);
	Root->Close(Root);

	EFI_HANDLE ShellHandle = NULL;
	Status = ManualLoad(FileBuf, FileSize, Self->DeviceHandle, ImageHandle, NULL, 0, &ShellHandle);
	if (EFI_ERROR(Status)) { Print(L"ManualLoad failed: %r\n", Status); goto bail; }

	Print(L"BootShim: launching shell via hooked StartImage\n");
	UINTN Esz = 0; CHAR16 *Edata = NULL;
	Status = HookedStartImage(ShellHandle, &Esz, &Edata);
	Print(L"BootShim: shell returned: %r\n", Status);

bail:
	Print(L"BootShim: bailed (%r). Press any key.\n", Status);
	UINTN KeyEvent;
	gST->ConIn->Reset(gST->ConIn, FALSE);
	gST->BootServices->WaitForEvent(1, &gST->ConIn->WaitForKey, &KeyEvent);
	return Status;
}
