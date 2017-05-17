/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2013 Will Andrews <will@firepipe.net>
 * Copyright (c) 2013, 2016 Jorgen Lundman <lundman@lundman.net>
 */

#undef _NTDDK_
#include <ntifs.h>
#include <ntddk.h>
#include <ntddscsi.h>
#include <scsi.h>
#include <ntddcdrm.h>
#include <ntdddisk.h>
#include <ntddstor.h>
#include <ntintsafe.h>
#include <mountmgr.h>
#include <Mountdev.h>
#include <ntddvol.h>

 // I have no idea what black magic is needed to get ntifs.h to define these

#ifndef FsRtlEnterFileSystem
#define FsRtlEnterFileSystem() { \
	KeEnterCriticalRegion();     \
}
#endif
#ifndef FsRtlExitFileSystem
#define FsRtlExitFileSystem() { \
    KeLeaveCriticalRegion();     \
}
#endif

#include <sys/cred.h>
#include <sys/vnode.h>
#include <sys/zfs_dir.h>
#include <sys/zfs_ioctl.h>
#include <sys/fs/zfs.h>
#include <sys/dmu.h>
#include <sys/dmu_objset.h>
#include <sys/spa.h>
#include <sys/txg.h>
#include <sys/dbuf.h>
#include <sys/zap.h>
#include <sys/sa.h>
#include <sys/zfs_vnops.h>
#include <sys/vfs.h>
#include <sys/vfs_opreg.h>
#include <sys/zfs_vfsops.h>
#include <sys/zfs_rlock.h>
#include <sys/zfs_ctldir.h>

//#include <sys/xattr.h>
//#include <sys/utfconv.h>
#include <sys/ubc.h>
#include <sys/callb.h>
#include <sys/unistd.h>
#include <sys/zfs_windows.h>
//#include <miscfs/fifofs/fifo.h>
//#include <miscfs/specfs/specdev.h>
//#include <vfs/vfs_support.h>
//#include <sys/ioccom.h>


PDEVICE_OBJECT ioctlDeviceObject = NULL;
PDEVICE_OBJECT diskDeviceObject = NULL;
PDEVICE_OBJECT fsDeviceObject = NULL;


#ifdef _KERNEL

DRIVER_INITIALIZE DriverEntry;

unsigned int debug_vnop_osx_printf = 0;
unsigned int zfs_vnop_ignore_negatives = 0;
unsigned int zfs_vnop_ignore_positives = 0;
unsigned int zfs_vnop_create_negatives = 1;
#endif

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#endif

#define	DECLARE_CRED(ap) \
	cred_t *cr;
#define	DECLARE_CONTEXT(ap) \
	caller_context_t *ct
#define	DECLARE_CRED_AND_CONTEXT(ap)	\
	DECLARE_CRED(ap);		\
	DECLARE_CONTEXT(ap)

//#define	dprintf if (debug_vnop_osx_printf) kprintf
//#define dprintf kprintf

//#define	dprintf(...) if (debug_vnop_osx_printf) {printf(__VA_ARGS__);delay(hz>>2);}
#ifdef _KERNEL
uint64_t vnop_num_reclaims = 0;
uint64_t vnop_num_vnodes = 0;
#endif

/*
 * zfs vfs operations.
 */


int zfs_vnop_lookup(PIRP Irp, PIO_STACK_LOCATION IrpSp, mount_t *zmo)
{
	int error;
	cred_t *cr = NULL;
	char filename[MAXNAMELEN];
	PFILE_OBJECT FileObject;
	ULONG outlen;
	struct vnode *dvp = NULL;
	struct vnode *vp;
	znode_t *zp = NULL;
	struct componentname cn;
	ULONG Options;
	BOOLEAN CreateDirectory;
	BOOLEAN NoIntermediateBuffering;
	BOOLEAN OpenDirectory;
	BOOLEAN IsPagingFile;
	BOOLEAN OpenTargetDirectory;
	BOOLEAN DirectoryFile;
	BOOLEAN NonDirectoryFile;
	BOOLEAN NoEaKnowledge;
	BOOLEAN DeleteOnClose;
	BOOLEAN OpenRequiringOplock;
	BOOLEAN TemporaryFile;
	BOOLEAN OpenRoot;
	ULONG CreateDisposition;
	zfsvfs_t *zfsvfs = vfs_fsprivate(zmo);

	FileObject = IrpSp->FileObject;
	Options = IrpSp->Parameters.Create.Options;

	dprintf("%s: enter\n", __func__);

	if (FileObject->RelatedFileObject != NULL) {
		FileObject->Vpb = FileObject->RelatedFileObject->Vpb;
	}

	DirectoryFile = BooleanFlagOn(Options, FILE_DIRECTORY_FILE);
	NonDirectoryFile = BooleanFlagOn(Options, FILE_NON_DIRECTORY_FILE);
	NoIntermediateBuffering = BooleanFlagOn(Options, FILE_NO_INTERMEDIATE_BUFFERING);
	NoEaKnowledge = BooleanFlagOn(Options, FILE_NO_EA_KNOWLEDGE);
	DeleteOnClose = BooleanFlagOn(Options, FILE_DELETE_ON_CLOSE);


	TemporaryFile = BooleanFlagOn(IrpSp->Parameters.Create.FileAttributes,
		FILE_ATTRIBUTE_TEMPORARY);

	CreateDisposition = (Options >> 24) & 0x000000ff;

	IsPagingFile = BooleanFlagOn(IrpSp->Flags, SL_OPEN_PAGING_FILE);
	OpenTargetDirectory = BooleanFlagOn(IrpSp->Flags, SL_OPEN_TARGET_DIRECTORY);

	CreateDirectory = (BOOLEAN)(DirectoryFile &&
		((CreateDisposition == FILE_CREATE) ||
		(CreateDisposition == FILE_OPEN_IF)));

	OpenDirectory = (BOOLEAN)(DirectoryFile &&
		((CreateDisposition == FILE_OPEN) ||
		(CreateDisposition == FILE_OPEN_IF)));

	// Convert incoming filename to utf8
	error = RtlUnicodeToUTF8N(filename,	MAXNAMELEN,	&outlen,
		FileObject->FileName.Buffer, FileObject->FileName.Length);

	if (error != STATUS_SUCCESS &&
		error != STATUS_SOME_NOT_MAPPED) {
		return STATUS_ILLEGAL_CHARACTER;
	}

	// Output string is only null terminated if input is, so do so now.
	filename[outlen] = 0;

	// Check if we are called as VFS_ROOT();
	OpenRoot = (strncmp("\\", filename, MAXNAMELEN) == 0 || strncmp("\\*", filename, MAXNAMELEN) == 0);

	if (FileObject->RelatedFileObject && FileObject->RelatedFileObject->FsContext) {
		dvp = FileObject->RelatedFileObject->FsContext;
	}

	if (OpenRoot) {

			error = zfs_zget(zfsvfs, zfsvfs->z_root, &zp);

			if (error == 0) {
				vp = ZTOV(zp);
				FileObject->FsContext = vp;
				VN_RELE(vp);

				// A valid lookup gets a ccb attached
				zfs_dirlist_t *zccb = kmem_alloc(sizeof(zfs_dirlist_t), KM_SLEEP);
				zccb->magic = ZFS_DIRLIST_MAGIC;
				zccb->uio_offset = 0;
				zccb->dir_eof = 0;
				IrpSp->FileObject->FsContext2 = zccb;


				Irp->IoStatus.Information = FILE_OPENED;
				return STATUS_SUCCESS;
			}

			Irp->IoStatus.Information = FILE_DOES_NOT_EXIST;
			return STATUS_OBJECT_PATH_NOT_FOUND;
	} // OpenRoot


	// We need to have a parent from here on.
	if (dvp == NULL) {
		char *brkt = NULL;
		char *word;
	//	DbgBreakPoint();
		// Iterate from root
		error = zfs_zget(zfsvfs, zfsvfs->z_root, &zp);
		if (error == 0) {
			dvp = ZTOV(zp);
			for (word = strtok_r(filename, "/\\", &brkt);
				word;
				word = strtok_r(NULL, "/\\", &brkt)) {

				cn.cn_nameiop = LOOKUP;
				cn.cn_flags = ISLASTCN;
				cn.cn_namelen = strlen(word);
				cn.cn_nameptr = word;
				error = zfs_lookup(dvp, word,
					&vp, &cn, cn.cn_nameiop, cr, /* flags */ 0);
				VN_RELE(dvp);

				if (error != 0) {
					dvp = NULL;
					break;
				}
				// If last lookup hit a non-directory type, we stop
				zp = VTOZ(vp);
				if (S_ISDIR(zp->z_mode)) {
					dvp = vp;
					vp = NULL;
				} else {
					VN_RELE(vp);
					break;
				} // is dir or not

			} // for word
			if (dvp) VN_RELE(dvp);
		}
		if (!dvp) {
			Irp->IoStatus.Information = FILE_DOES_NOT_EXIST;
			return STATUS_OBJECT_PATH_NOT_FOUND;
		}
	}

	cn.cn_nameiop = LOOKUP;
	cn.cn_flags = ISLASTCN;
	cn.cn_namelen = strlen(filename);
	cn.cn_nameptr = filename;
			
	// Here we have "dvp" of the directory. 
	// "vp" if the final part was a file.
	if (CreateDirectory) {
		return STATUS_NOT_IMPLEMENTED;
	}

	// If they requested just directory, fail non directories
	if (DirectoryFile && vp != NULL) {
		dprintf("%s: asked for directory but found file\n", __func__);
		Irp->IoStatus.Information = FILE_DOES_NOT_EXIST;
		return STATUS_OBJECT_PATH_NOT_FOUND;
	}

	// Asked for non-directory, but we got directory
	if (NonDirectoryFile && vp == NULL) {
		Irp->IoStatus.Information = FILE_DOES_NOT_EXIST;
		return STATUS_OBJECT_PATH_NOT_FOUND;
	}

	// Just open it, if the open was to a directory, add ccb
	ASSERT(IrpSp->FileObject->FsContext == NULL);
	if (vp == NULL) {
		zfs_dirlist_t *zccb = kmem_alloc(sizeof(zfs_dirlist_t), KM_SLEEP);
		ASSERT(IrpSp->FileObject->FsContext2 == NULL);
		zccb->magic = ZFS_DIRLIST_MAGIC;
		zccb->uio_offset = 0;
		zccb->dir_eof = 0;
		IrpSp->FileObject->FsContext2 = zccb;
		FileObject->FsContext = dvp;
	} else {
		FileObject->FsContext = vp;
	}

	Irp->IoStatus.Information = FILE_OPENED;
	return STATUS_SUCCESS;
}


/*
 */
void
getnewvnode_reserve(int num)
{
}

void
getnewvnode_drop_reserve()
{
}

/*
 * Get new vnode for znode.
 *
 * This function uses zp->z_zfsvfs, zp->z_mode, zp->z_flags, zp->z_id and sets
 * zp->z_vnode and zp->z_vid.
 */
int
zfs_znode_getvnode(znode_t *zp, zfsvfs_t *zfsvfs)
{
	struct vnode *vp = NULL;

	dprintf("getvnode zp %p with vp %p zfsvfs %p vfs %p\n", zp, vp,
	    zfsvfs, zfsvfs->z_vfs);

	if (zp->z_vnode)
		panic("zp %p vnode already set\n", zp->z_vnode);


	/*
	 * vnode_create() has a habit of calling both vnop_reclaim() and
	 * vnop_fsync(), which can create havok as we are already holding locks.
	 */

	vp = kmem_alloc(sizeof(*vp), KM_SLEEP);
	vp->v_data = zp;
	vp->v_type = IFTOVT((mode_t)zp->z_mode);
	atomic_inc_64(&vnop_num_vnodes);

	dprintf("Assigned zp %p with vp %p\n", zp, vp);

	zp->z_vid = vnode_vid(vp);
	zp->z_vnode = vp;

	return (0);
}


NTSTATUS dev_ioctl(PDEVICE_OBJECT DeviceObject, ULONG ControlCode, PVOID InputBuffer, ULONG InputBufferSize,
	PVOID OutputBuffer, ULONG OutputBufferSize, BOOLEAN Override, IO_STATUS_BLOCK* iosb)
{
	PIRP Irp;
	KEVENT Event;
	NTSTATUS Status;
	PIO_STACK_LOCATION Stack;
	IO_STATUS_BLOCK IoStatus;

	KeInitializeEvent(&Event, NotificationEvent, FALSE);

	Irp = IoBuildDeviceIoControlRequest(ControlCode,
		DeviceObject,
		InputBuffer,
		InputBufferSize,
		OutputBuffer,
		OutputBufferSize,
		FALSE,
		&Event,
		&IoStatus);

	if (!Irp) return STATUS_INSUFFICIENT_RESOURCES;

	if (Override) {
		Stack = IoGetNextIrpStackLocation(Irp);
		Stack->Flags |= SL_OVERRIDE_VERIFY_VOLUME;
	}

	Status = IoCallDriver(DeviceObject, Irp);

	if (Status == STATUS_PENDING) {
		KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
		Status = IoStatus.Status;
	}

	if (iosb)
		*iosb = IoStatus;

	return Status;
}


int zfs_vnop_mount(PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
	PDEVICE_OBJECT DeviceToMount;
	DeviceToMount = IrpSp->Parameters.MountVolume.DeviceObject;

	dprintf("*** mount request for %p\n", DeviceToMount);

	NTSTATUS Status;
	MOUNTDEV_NAME mdn, *mdn2;
	ULONG mdnsize;

	Status = dev_ioctl(DeviceToMount, IOCTL_MOUNTDEV_QUERY_DEVICE_NAME, NULL, 0, &mdn, sizeof(MOUNTDEV_NAME), TRUE, NULL);
	if (Status != STATUS_SUCCESS && Status != STATUS_BUFFER_OVERFLOW)
		return STATUS_UNRECOGNIZED_VOLUME;

	mdnsize = offsetof(MOUNTDEV_NAME, Name[0]) + mdn.NameLength;
	mdn2 = kmem_alloc(mdnsize, KM_SLEEP);

	dprintf("mount strlen %d\n", mdn.NameLength);

	Status = dev_ioctl(DeviceToMount, IOCTL_MOUNTDEV_QUERY_DEVICE_NAME, NULL, 0, mdn2, mdnsize, TRUE, NULL);
	if (Status != STATUS_SUCCESS)
		goto out;

	dprintf("mount about '%.*S'\n", mdn2->NameLength/sizeof(WCHAR), mdn2->Name);
#if 0
	ANSI_STRING ansi;
	UNICODE_STRING uni;
	RtlUnicodeStringInit(&uni, mdn2->Name);
	RtlUnicodeStringToAnsiString(&ansi, &uni, TRUE);
	dprintf("mount about '%s'\n", ansi.Buffer);
	RtlFreeAnsiString(&ansi);
#endif
out:
	kmem_free(mdn2, mdnsize);
	return STATUS_UNRECOGNIZED_VOLUME;
}


char *major2str(int major, int minor)
{
	switch (major) {
	case IRP_MJ_CREATE:
		return "IRP_MJ_CREATE";
	case IRP_MJ_CREATE_NAMED_PIPE:
		return "IRP_MJ_CREATE_NAMED_PIPE";
	case IRP_MJ_CLOSE:
		return "IRP_MJ_CLOSE";
	case IRP_MJ_READ:
		return "IRP_MJ_READ";
	case IRP_MJ_WRITE:
		return "IRP_MJ_WRITE";
	case IRP_MJ_QUERY_INFORMATION:
		return "IRP_MJ_QUERY_INFORMATION";
	case IRP_MJ_SET_INFORMATION:
		return "IRP_MJ_SET_INFORMATION";
	case IRP_MJ_QUERY_EA:
		return "IRP_MJ_QUERY_EA";
	case IRP_MJ_SET_EA:
		return "IRP_MJ_SET_EA";
	case IRP_MJ_FLUSH_BUFFERS:
		return "IRP_MJ_FLUSH_BUFFERS";
	case IRP_MJ_QUERY_VOLUME_INFORMATION:
		return "IRP_MJ_QUERY_VOLUME_INFORMATION";
	case IRP_MJ_SET_VOLUME_INFORMATION:
		return "IRP_MJ_SET_VOLUME_INFORMATION";
	case IRP_MJ_DIRECTORY_CONTROL:
		switch (minor) {
		case IRP_MN_NOTIFY_CHANGE_DIRECTORY:
			return "IRP_MJ_DIRECTORY_CONTROL(IRP_MN_NOTIFY_CHANGE_DIRECTORY)";
		case IRP_MN_QUERY_DIRECTORY:
			return "IRP_MJ_DIRECTORY_CONTROL(IRP_MN_QUERY_DIRECTORY)";
		}
		return "IRP_MJ_DIRECTORY_CONTROL";
	case IRP_MJ_FILE_SYSTEM_CONTROL:
		switch (minor) {
		case IRP_MN_KERNEL_CALL:
			return "IRP_MJ_FILE_SYSTEM_CONTROL(IRP_MN_KERNEL_CALL)";
		case IRP_MN_MOUNT_VOLUME:
			return "IRP_MJ_FILE_SYSTEM_CONTROL(IRP_MN_MOUNT_VOLUME)";
		case IRP_MN_USER_FS_REQUEST:
			return "IRP_MJ_FILE_SYSTEM_CONTROL(IRP_MN_USER_FS_REQUEST)";
		case IRP_MN_VERIFY_VOLUME:
			return "IRP_MJ_FILE_SYSTEM_CONTROL(IRP_MN_VERIFY_VOLUME)";
		case IRP_MN_LOAD_FILE_SYSTEM:
			return "IRP_MJ_FILE_SYSTEM_CONTROL(IRP_MN_LOAD_FILE_SYSTEM)";
		}
		return "IRP_MJ_FILE_SYSTEM_CONTROL";
	case IRP_MJ_DEVICE_CONTROL:
		return "IRP_MJ_DEVICE_CONTROL";
	case IRP_MJ_INTERNAL_DEVICE_CONTROL:
		return "IRP_MJ_INTERNAL_DEVICE_CONTROL";
	case IRP_MJ_SHUTDOWN:
		return "IRP_MJ_SHUTDOWN";
	case IRP_MJ_LOCK_CONTROL:
		switch (minor) {
		case IRP_MN_LOCK:
			return "IRP_MJ_LOCK_CONTROL(IRP_MN_LOCK)";
		case IRP_MN_UNLOCK_ALL:
			return "IRP_MJ_LOCK_CONTROL(IRP_MN_UNLOCK_ALL)";
		case IRP_MN_UNLOCK_ALL_BY_KEY:
			return "IRP_MJ_LOCK_CONTROL(IRP_MN_UNLOCK_ALL_BY_KEY)";
		case IRP_MN_UNLOCK_SINGLE:
			return "IRP_MJ_LOCK_CONTROL(IRP_MN_UNLOCK_SINGLE)";
		}
		return "IRP_MJ_LOCK_CONTROL";
	case IRP_MJ_CLEANUP:
		return "IRP_MJ_CLEANUP";
	case IRP_MJ_CREATE_MAILSLOT:
		return "IRP_MJ_CREATE_MAILSLOT";
	case IRP_MJ_QUERY_SECURITY:
		return "IRP_MJ_QUERY_SECURITY";
	case IRP_MJ_SET_SECURITY:
		return "IRP_MJ_SET_SECURITY";
	case IRP_MJ_POWER:
		return "IRP_MJ_POWER";
	case IRP_MJ_SYSTEM_CONTROL:
		return "IRP_MJ_SYSTEM_CONTROL";
	case IRP_MJ_DEVICE_CHANGE:
		return "IRP_MJ_DEVICE_CHANGE";
	case IRP_MJ_QUERY_QUOTA:
		return "IRP_MJ_QUERY_QUOTA";
	case IRP_MJ_SET_QUOTA:
		return "IRP_MJ_SET_QUOTA";
	case IRP_MJ_PNP:
		switch (minor) {
		case IRP_MN_START_DEVICE:
			return "IRP_MJ_PNP(IRP_MN_START_DEVICE)";
		case IRP_MN_QUERY_REMOVE_DEVICE:
			return "IRP_MJ_PNP(IRP_MN_QUERY_REMOVE_DEVICE)";
		case IRP_MN_REMOVE_DEVICE:
			return "IRP_MJ_PNP(IRP_MN_REMOVE_DEVICE)";
		case IRP_MN_CANCEL_REMOVE_DEVICE:
			return "IRP_MJ_PNP(IRP_MN_CANCEL_REMOVE_DEVICE)";
		case IRP_MN_STOP_DEVICE:
			return "IRP_MJ_PNP(IRP_MN_STOP_DEVICE)";
		case IRP_MN_QUERY_STOP_DEVICE:
			return "IRP_MJ_PNP(IRP_MN_QUERY_STOP_DEVICE)";
		case IRP_MN_CANCEL_STOP_DEVICE:
			return "IRP_MJ_PNP(IRP_MN_CANCEL_STOP_DEVICE)";
		case IRP_MN_QUERY_DEVICE_RELATIONS:
			return "IRP_MJ_PNP(IRP_MN_QUERY_DEVICE_RELATIONS)";
		case IRP_MN_QUERY_INTERFACE:
			return "IRP_MJ_PNP(IRP_MN_QUERY_INTERFACE)";
		case IRP_MN_QUERY_RESOURCES:
			return "IRP_MJ_PNP(IRP_MN_QUERY_RESOURCES)";
		case IRP_MN_QUERY_RESOURCE_REQUIREMENTS:
			return "IRP_MJ_PNP(IRP_MN_QUERY_RESOURCE_REQUIREMENTS)";
		case IRP_MN_QUERY_CAPABILITIES:
			return "IRP_MJ_PNP(IRP_MN_QUERY_CAPABILITIES)";
		case IRP_MN_QUERY_DEVICE_TEXT:
			return "IRP_MJ_PNP(IRP_MN_QUERY_DEVICE_TEXT)";
		case IRP_MN_FILTER_RESOURCE_REQUIREMENTS:
			return "IRP_MJ_PNP(IRP_MN_FILTER_RESOURCE_REQUIREMENTS)";
		case IRP_MN_READ_CONFIG:
			return "IRP_MJ_PNP(IRP_MN_READ_CONFIG)";
		case IRP_MN_WRITE_CONFIG:
			return "IRP_MJ_PNP(IRP_MN_WRITE_CONFIG)";
		case IRP_MN_EJECT:
			return "IRP_MJ_PNP(IRP_MN_EJECT)";
		case IRP_MN_SET_LOCK:
			return "IRP_MJ_PNP(IRP_MN_SET_LOCK)";
		case IRP_MN_QUERY_ID:
			return "IRP_MJ_PNP(IRP_MN_QUERY_ID)";
		case IRP_MN_QUERY_PNP_DEVICE_STATE:
			return "IRP_MJ_PNP(IRP_MN_QUERY_PNP_DEVICE_STATE)";
		case IRP_MN_QUERY_BUS_INFORMATION:
			return "IRP_MJ_PNP(IRP_MN_QUERY_BUS_INFORMATION)";
		case IRP_MN_DEVICE_USAGE_NOTIFICATION:
			return "IRP_MJ_PNP(IRP_MN_DEVICE_USAGE_NOTIFICATION)";
		case IRP_MN_SURPRISE_REMOVAL: // SUPPLIES!
			return "IRP_MJ_PNP(IRP_MN_SURPRISE_REMOVAL)";
		}
		return "IRP_MJ_PNP";
	default:
		break;
	}
	return "Unknown";
}


NTSTATUS QueryCapabilities(PDEVICE_OBJECT DeviceObject, PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
	NTSTATUS				Status;
	PDEVICE_CAPABILITIES	DeviceCapabilities;
	DeviceCapabilities = IrpSp->Parameters.DeviceCapabilities.Capabilities;
	DeviceCapabilities->SurpriseRemovalOK = TRUE;
	DeviceCapabilities->LockSupported = TRUE;
	DeviceCapabilities->EjectSupported = TRUE;
	DeviceCapabilities->Removable = TRUE;
	DeviceCapabilities->DockDevice = FALSE;
	DeviceCapabilities->D1Latency = DeviceCapabilities->D2Latency = DeviceCapabilities->D3Latency = 0;
	DeviceCapabilities->NoDisplayInUI = 0;
	Irp->IoStatus.Information = sizeof(DEVICE_CAPABILITIES);

	return STATUS_SUCCESS;
}

// THIS IS THE PNP DEVICE ID
NTSTATUS pnp_query_id(PDEVICE_OBJECT DeviceObject, PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
	mount_t *zmo;

	dprintf("%s: query id type %d\n", __func__, IrpSp->Parameters.QueryId.IdType);

	zmo = (mount_t *)DeviceObject->DeviceExtension;

	Irp->IoStatus.Information = ExAllocatePoolWithTag(PagedPool, zmo->bus_name.Length + sizeof(UNICODE_NULL), '!OIZ');
	if (Irp->IoStatus.Information == NULL) return STATUS_NO_MEMORY;

	RtlCopyMemory(Irp->IoStatus.Information, zmo->bus_name.Buffer, zmo->bus_name.Length);
	dprintf("replying with '%.*S'\n", zmo->uuid.Length/sizeof(WCHAR), Irp->IoStatus.Information);

	return STATUS_SUCCESS;
}

NTSTATUS pnp_device_state(PDEVICE_OBJECT DeviceObject, PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
	dprintf("%s:\n", __func__);

	Irp->IoStatus.Information |= PNP_DEVICE_NOT_DISABLEABLE;

	return STATUS_SUCCESS;
}


NTSTATUS ioctl_query_device_name(PDEVICE_OBJECT DeviceObject, PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
	// Return name in MOUNTDEV_NAME
	PMOUNTDEV_NAME name;
	mount_t *zmo;

	if (IrpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(MOUNTDEV_NAME)) {
		Irp->IoStatus.Information = sizeof(MOUNTDEV_NAME);
		return STATUS_BUFFER_TOO_SMALL;
	}

	zmo = (mount_t *)DeviceObject->DeviceExtension;

	name = Irp->AssociatedIrp.SystemBuffer;

	name->NameLength = zmo->device_name.Length;
	if (IrpSp->Parameters.DeviceIoControl.OutputBufferLength < offsetof(MOUNTDEV_NAME, Name[0]) + name->NameLength) {
		Irp->IoStatus.Information = sizeof(MOUNTDEV_NAME);
		return STATUS_BUFFER_OVERFLOW;
	}
	//name = ExAllocatePoolWithTag(PagedPool, sizeof(MOUNTDEV_NAME) + zmo->name.Length, '!OIZ');
	//if (name == NULL) return STATUS_NO_MEMORY;
	RtlCopyMemory(name->Name, zmo->device_name.Buffer, zmo->device_name.Length);

	//Irp->IoStatus.Information = name;
	Irp->IoStatus.Information = offsetof(MOUNTDEV_NAME, Name[0]) + name->NameLength;
	dprintf("replying with '%.*S'\n", name->NameLength/sizeof(WCHAR), name->Name);

	return STATUS_SUCCESS;
}

NTSTATUS ioctl_query_unique_id(PDEVICE_OBJECT DeviceObject, PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
	PMOUNTDEV_UNIQUE_ID uniqueId;
	WCHAR				deviceName[MAXIMUM_FILENAME_LENGTH];
	ULONG				bufferLength = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
	mount_t *zmo;

	zmo = (mount_t *)DeviceObject->DeviceExtension;

	if (bufferLength < sizeof(MOUNTDEV_UNIQUE_ID)) {
		Irp->IoStatus.Information = sizeof(MOUNTDEV_UNIQUE_ID);
		return STATUS_BUFFER_TOO_SMALL;
	}

	uniqueId = (PMOUNTDEV_UNIQUE_ID)Irp->AssociatedIrp.SystemBuffer;
	ASSERT(uniqueId != NULL);

	//uniqueId->UniqueIdLength = zmo->uuid.Length + 1 * sizeof(WCHAR); // includes null char
	uniqueId->UniqueIdLength = zmo->symlink_name.Length;

	if (sizeof(USHORT) + uniqueId->UniqueIdLength < bufferLength) {
		RtlCopyMemory((PCHAR)uniqueId->UniqueId, zmo->symlink_name.Buffer, zmo->symlink_name.Length);
//		uniqueId->UniqueId[zmo->uuid.Length] = UNICODE_NULL;
		Irp->IoStatus.Information = FIELD_OFFSET(MOUNTDEV_UNIQUE_ID, UniqueId[0]) +
			uniqueId->UniqueIdLength;
		dprintf("replying with '%.*S'\n", uniqueId->UniqueIdLength/sizeof(WCHAR), uniqueId->UniqueId);
		return STATUS_SUCCESS;
	} else {
		Irp->IoStatus.Information = sizeof(MOUNTDEV_UNIQUE_ID);
		return STATUS_BUFFER_OVERFLOW;
	}
}


NTSTATUS query_volume_information(PDEVICE_OBJECT DeviceObject, PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
	NTSTATUS Status;

	Status = STATUS_NOT_IMPLEMENTED;

	switch (IrpSp->Parameters.QueryVolume.FsInformationClass) {

	case FileFsAttributeInformation:   // ***
		{
			dprintf("%s: FileFsAttributeInformation\n", __func__);
			if (IrpSp->Parameters.QueryVolume.Length < sizeof(FILE_FS_ATTRIBUTE_INFORMATION)) {
				Irp->IoStatus.Information = sizeof(FILE_FS_ATTRIBUTE_INFORMATION);
				Status = STATUS_BUFFER_TOO_SMALL;
				break;
			}
			if (IrpSp->Parameters.QueryVolume.Length < sizeof(FILE_FS_ATTRIBUTE_INFORMATION) + sizeof(VOLUME_LABEL)) {
				Irp->IoStatus.Information = sizeof(FILE_FS_ATTRIBUTE_INFORMATION) + sizeof(VOLUME_LABEL);
				Status = STATUS_BUFFER_OVERFLOW;
				break;
			}
			FILE_FS_ATTRIBUTE_INFORMATION *out = Irp->AssociatedIrp.SystemBuffer;
			out->FileSystemAttributes = FILE_CASE_PRESERVED_NAMES | FILE_CASE_SENSITIVE_SEARCH | FILE_NAMED_STREAMS |
				FILE_PERSISTENT_ACLS | FILE_SUPPORTS_OBJECT_IDS | FILE_SUPPORTS_REPARSE_POINTS | FILE_SUPPORTS_SPARSE_FILES | FILE_VOLUME_QUOTAS;
			wcscpy(out->FileSystemName, VOLUME_LABEL);
			out->FileSystemNameLength = sizeof(VOLUME_LABEL);
			out->MaximumComponentNameLength = PATH_MAX;

			Irp->IoStatus.Information = sizeof(FILE_FS_ATTRIBUTE_INFORMATION) + sizeof(VOLUME_LABEL);
			Status = STATUS_SUCCESS;
		}	
		break;
	case FileFsControlInformation:
		dprintf("%s: FileFsControlInformation\n", __func__);
		break;
	case FileFsDeviceInformation:
		dprintf("%s: FileFsDeviceInformation\n", __func__);
		break;
	case FileFsDriverPathInformation:
		dprintf("%s: FileFsDriverPathInformation\n", __func__);
		break;
	case FileFsFullSizeInformation:   //**
		dprintf("%s: FileFsFullSizeInformation\n", __func__);
		if (IrpSp->Parameters.QueryVolume.Length < sizeof(FILE_FS_FULL_SIZE_INFORMATION)) {
			Irp->IoStatus.Information = sizeof(FILE_FS_FULL_SIZE_INFORMATION);
			Status = STATUS_BUFFER_TOO_SMALL;
			break;
		}
		FILE_FS_FULL_SIZE_INFORMATION *out = Irp->AssociatedIrp.SystemBuffer;
		out->TotalAllocationUnits.QuadPart = 1234;
		out->ActualAvailableAllocationUnits.QuadPart = 1233;
		out->CallerAvailableAllocationUnits.QuadPart = 1232;
		out->BytesPerSector = 512;
		out->SectorsPerAllocationUnit = 1;
		Irp->IoStatus.Information = sizeof(FILE_FS_FULL_SIZE_INFORMATION);
		Status = STATUS_SUCCESS;
		break;
	case FileFsObjectIdInformation:
		dprintf("%s: FileFsObjectIdInformation\n", __func__);
		break;
	case FileFsSizeInformation:
		dprintf("%s: FileFsSizeInformation\n", __func__);
		break;
	case FileFsVolumeInformation:    // *** 
		{
			dprintf("%s: FileFsVolumeInformation\n", __func__);
			if (IrpSp->Parameters.QueryVolume.Length < sizeof(FILE_FS_VOLUME_INFORMATION)) {
				Irp->IoStatus.Information = sizeof(FILE_FS_VOLUME_INFORMATION);
				Status = STATUS_BUFFER_TOO_SMALL;
				break;
			}
			if (IrpSp->Parameters.QueryVolume.Length < sizeof(FILE_FS_VOLUME_INFORMATION) + sizeof(VOLUME_LABEL)) {
				Irp->IoStatus.Information = sizeof(FILE_FS_VOLUME_INFORMATION) + sizeof(VOLUME_LABEL);
				Status = STATUS_BUFFER_OVERFLOW;
				break;
			}
			FILE_FS_VOLUME_INFORMATION *out = Irp->AssociatedIrp.SystemBuffer;
			out->VolumeSerialNumber = ZFS_SERIAL;
			out->SupportsObjects = FALSE;
			out->VolumeCreationTime.QuadPart = gethrtime();
			out->VolumeLabelLength = wcslen(VOLUME_LABEL) * sizeof(WCHAR);
			RtlStringCchCopyW(out->VolumeLabel,
				sizeof(out->VolumeLabel) / sizeof(WCHAR),
				VOLUME_LABEL);

			Irp->IoStatus.Information = sizeof(FILE_FS_VOLUME_INFORMATION) + sizeof(VOLUME_LABEL);
			Status = STATUS_SUCCESS;
		}
		break;
	case FileFsSectorSizeInformation:
		dprintf("%s: FileFsSectorSizeInformation\n", __func__);
		Status = STATUS_NOT_IMPLEMENTED;
		break;
	default:
		dprintf("%s: unknown class 0x%x\n", __func__, IrpSp->Parameters.QueryVolume.FsInformationClass);
		Status = STATUS_NOT_IMPLEMENTED;
		break;
	}

	return Status;
}


NTSTATUS lock_control(PDEVICE_OBJECT DeviceObject, PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
	NTSTATUS Status = STATUS_SUCCESS;

	dprintf("%s: FileObject %p flags 0x%x %s %s\n", __func__,
		IrpSp->FileObject, IrpSp->Flags,
		IrpSp->Flags & SL_EXCLUSIVE_LOCK ? "Exclusive" : "Shared",
		IrpSp->Flags & SL_FAIL_IMMEDIATELY ? "Nowait" : "Wait"
	);

	return Status;
}

NTSTATUS file_basic_information(PDEVICE_OBJECT DeviceObject, PIRP Irp, PIO_STACK_LOCATION IrpSp, FILE_BASIC_INFORMATION *basic)
{
	dprintf("   %s\n", __func__);
	basic->ChangeTime.QuadPart = gethrtime();
	basic->CreationTime.QuadPart = gethrtime();
	basic->LastAccessTime.QuadPart = gethrtime();
	basic->LastWriteTime.QuadPart = gethrtime();
	basic->FileAttributes = FILE_ATTRIBUTE_NORMAL;
	if (IrpSp->FileObject && IrpSp->FileObject->FsContext) {
		struct vnode *vp = IrpSp->FileObject->FsContext;
		VN_HOLD(vp);
		znode_t *zp = VTOZ(vp);
		if (S_ISDIR(zp->z_mode)) 
			basic->FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
		else
			basic->FileAttributes = FILE_ATTRIBUTE_NORMAL;
		VN_RELE(vp);
	}
	return STATUS_SUCCESS;
}

NTSTATUS file_standard_information(PDEVICE_OBJECT DeviceObject, PIRP Irp, PIO_STACK_LOCATION IrpSp, FILE_STANDARD_INFORMATION *standard)
{
	dprintf("   %s\n", __func__);
	standard->Directory = TRUE;
	standard->AllocationSize.QuadPart = 512;  // space taken on disk, multiples of block size
	standard->EndOfFile.QuadPart = 512;       // byte size of file
	standard->DeletePending = FALSE;
	standard->NumberOfLinks = 5;
	return STATUS_SUCCESS;
}

NTSTATUS file_position_information(PDEVICE_OBJECT DeviceObject, PIRP Irp, PIO_STACK_LOCATION IrpSp, FILE_POSITION_INFORMATION *position)
{
	dprintf("   %s\n", __func__);
	position->CurrentByteOffset.QuadPart = 0;
	return STATUS_SUCCESS;
}

NTSTATUS file_name_information(PDEVICE_OBJECT DeviceObject, PIRP Irp, PIO_STACK_LOCATION IrpSp, FILE_NAME_INFORMATION *name)
{
	UNICODE_STRING str;
	RtlInitUnicodeString(&str, L"\\");
	PFILE_OBJECT FileObject = IrpSp->FileObject;

	if (FileObject == NULL || FileObject->FsContext == NULL)
		return STATUS_INVALID_PARAMETER;

	struct vnode *vp = FileObject->FsContext;
	znode_t *zp = VTOZ(vp);
	char strname[MAXPATHLEN + 2];
	int error;
	uint64_t parent;
	zfsvfs_t *zfsvfs = zp->z_zfsvfs;

	VN_HOLD(vp);
	VERIFY(sa_lookup(zp->z_sa_hdl, SA_ZPL_PARENT(zfsvfs),
		&parent, sizeof(parent)) == 0);

	error = zap_value_search(zfsvfs->z_os, parent, zp->z_id,
		ZFS_DIRENT_OBJ(-1ULL), strname);
	
	VN_RELE(vp);

	if (error)
		return STATUS_FILE_INVALID;

	// Convert name
	error = RtlUTF8ToUnicodeN(NULL, 0, &name->FileNameLength, strname, strlen(strname));

	dprintf("%s: remaining space %d str.len %d struct size %d\n", __func__, IrpSp->Parameters.QueryFile.Length,
		str.Length, sizeof(FILE_NAME_INFORMATION));

	Irp->IoStatus.Information = sizeof(FILE_NAME_INFORMATION) + name->FileNameLength;
	if (IrpSp->Parameters.QueryFile.Length < offsetof(FILE_NAME_INFORMATION, FileName[0]) + name->FileNameLength) {
		return STATUS_BUFFER_OVERFLOW;
	}

	error = RtlUTF8ToUnicodeN(name->FileName, name->FileNameLength, &name->FileNameLength, strname, strlen(strname));

	dprintf("%s: hardcoded name of %wZ\n", __func__, &str);
	return STATUS_SUCCESS;
}


NTSTATUS query_information(PDEVICE_OBJECT DeviceObject, PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
	NTSTATUS Status = STATUS_NOT_IMPLEMENTED;

	switch (IrpSp->Parameters.QueryFile.FileInformationClass) {
			
	case FileAllInformation: 
		dprintf("%s: FileAllInformation\n", __func__);
		if (IrpSp->Parameters.QueryFile.Length < sizeof(FILE_ALL_INFORMATION)) {
			Irp->IoStatus.Information = sizeof(FILE_ALL_INFORMATION);  // We should send Plus Filename here, to be nice, but this doesnt happen
			Status = STATUS_BUFFER_TOO_SMALL;
			break;
		}
		FILE_ALL_INFORMATION *all = Irp->AssociatedIrp.SystemBuffer;
		Status = file_basic_information(DeviceObject, Irp, IrpSp, &all->BasicInformation);
		if (Status != STATUS_SUCCESS) break;
		Status = file_standard_information(DeviceObject, Irp, IrpSp, &all->StandardInformation);
		if (Status != STATUS_SUCCESS) break;
		Status = file_position_information(DeviceObject, Irp, IrpSp, &all->PositionInformation);
		if (Status != STATUS_SUCCESS) break;

		all->AlignmentInformation.AlignmentRequirement = 512;

		// Remove the size of everything up to FILE_NAME_INFORMATION so we only have that
		// and any extra room for the filename to fill.
		IrpSp->Parameters.QueryFile.Length -= offsetof(FILE_ALL_INFORMATION, NameInformation);
		Status = file_name_information(DeviceObject, Irp, IrpSp, &all->NameInformation);

		Irp->IoStatus.Information = sizeof(FILE_ALL_INFORMATION) + all->NameInformation.FileNameLength;
		break;
	case FileAttributeTagInformation:
		dprintf("%s: FileAttributeTagInformation\n", __func__);
		break;
	case FileBasicInformation:
		dprintf("%s: FileBasicInformation\n", __func__);	
		if (IrpSp->Parameters.QueryFile.Length < sizeof(FILE_BASIC_INFORMATION)) {
			Irp->IoStatus.Information = sizeof(FILE_BASIC_INFORMATION);
			Status = STATUS_BUFFER_TOO_SMALL;
			break;
		}
		Status = file_basic_information(DeviceObject, Irp, IrpSp, Irp->AssociatedIrp.SystemBuffer);
		Irp->IoStatus.Information = sizeof(FILE_BASIC_INFORMATION);
		break;
	case FileCompressionInformation:
		dprintf("%s: FileCompressionInformation\n", __func__);
		break;
	case FileEaInformation:
		dprintf("%s: FileEaInformation\n", __func__);
		break;
	case FileInternalInformation:
		dprintf("%s: FileInternalInformation\n", __func__);
		break;
	case FileNameInformation:
		dprintf("%s: FileNameInformation\n", __func__);
		if (IrpSp->Parameters.QueryFile.Length < sizeof(FILE_NAME_INFORMATION)) {
			Irp->IoStatus.Information = sizeof(FILE_NAME_INFORMATION);
			Status = STATUS_BUFFER_TOO_SMALL;
			break;
		}
		FILE_NAME_INFORMATION *name = Irp->AssociatedIrp.SystemBuffer;
		Status = file_name_information(DeviceObject, Irp, IrpSp, name);
		Irp->IoStatus.Information = sizeof(FILE_NAME_INFORMATION) + name->FileNameLength;
		break;
	case FileNetworkOpenInformation:   
		dprintf("%s: FileNetworkOpenInformation\n", __func__);
		break;
	case FilePositionInformation:
		dprintf("%s: FilePositionInformation\n", __func__);
		if (IrpSp->Parameters.QueryFile.Length < sizeof(FILE_POSITION_INFORMATION)) {
			Irp->IoStatus.Information = sizeof(FILE_POSITION_INFORMATION);
			Status = STATUS_BUFFER_TOO_SMALL;
			break;
		}
		Status = file_position_information(DeviceObject, Irp, IrpSp, Irp->AssociatedIrp.SystemBuffer);
		Irp->IoStatus.Information = sizeof(FILE_POSITION_INFORMATION);
		break;
	case FileStandardInformation:
		dprintf("%s: FileStandardInformation\n", __func__);
		if (IrpSp->Parameters.QueryFile.Length < sizeof(FILE_STANDARD_INFORMATION)) {
			Irp->IoStatus.Information = sizeof(FILE_STANDARD_INFORMATION);
			Status = STATUS_BUFFER_TOO_SMALL;
			break;
		}
		Status = file_standard_information(DeviceObject, Irp, IrpSp, Irp->AssociatedIrp.SystemBuffer);
		Irp->IoStatus.Information = sizeof(FILE_STANDARD_INFORMATION);
		break;
	case FileStreamInformation:
		dprintf("%s: FileStreamInformation\n", __func__);
		break;
	case FileHardLinkInformation:
		dprintf("%s: FileHardLinkInformation\n", __func__);
		break;
	default:
		dprintf("%s: unknown class 0x%x\n", __func__, IrpSp->Parameters.QueryFile.FileInformationClass);
		break;
	}

	return Status;
}

NTSTATUS user_fs_request(PDEVICE_OBJECT DeviceObject, PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
	NTSTATUS Status = STATUS_NOT_IMPLEMENTED;

	switch (IrpSp->Parameters.FileSystemControl.FsControlCode) {

	case FSCTL_LOCK_VOLUME:
		dprintf("    FSCTL_LOCK_VOLUME\n");
		Status = STATUS_SUCCESS;
		break;
	case FSCTL_UNLOCK_VOLUME:
		dprintf("    FSCTL_UNLOCK_VOLUME\n");
		Status = STATUS_SUCCESS;
		break;
	case FSCTL_DISMOUNT_VOLUME:
		dprintf("    FSCTL_DISMOUNT_VOLUME\n");
		break;
	case FSCTL_MARK_VOLUME_DIRTY:
		dprintf("    FSCTL_MARK_VOLUME_DIRTY\n");
		Status = STATUS_SUCCESS;
		break;
	case FSCTL_IS_VOLUME_MOUNTED:
		dprintf("    FSCTL_IS_VOLUME_MOUNTED\n");
		Status = STATUS_SUCCESS;
		break;
	case FSCTL_IS_PATHNAME_VALID:
		dprintf("    FSCTL_IS_PATHNAME_VALID\n");
		Status = STATUS_SUCCESS;
		break;
	case FSCTL_GET_RETRIEVAL_POINTERS:
		dprintf("    FSCTL_GET_RETRIEVAL_POINTERS\n");
		Status = STATUS_INVALID_PARAMETER;
		break;
	case FSCTL_IS_VOLUME_DIRTY:
		dprintf("    FSCTL_IS_VOLUME_DIRTY\n");
		Status = STATUS_INVALID_PARAMETER;
		break;
	case FSCTL_GET_REPARSE_POINT:
		dprintf("    FSCTL_GET_REPARSE_POINT\n");
		Status = STATUS_NOT_A_REPARSE_POINT;
		break;
	default:
		dprintf("%s: unknown class 0x%x\n", __func__, IrpSp->Parameters.FileSystemControl.FsControlCode);
		break;
	}

	return Status;
}

NTSTATUS query_directory_FileFullDirectoryInformation(PDEVICE_OBJECT DeviceObject, PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
	//FILE_FULL_DIR_INFORMATION *outptr = Irp->UserBuffer;
	int flag_index_specified     = IrpSp->Flags & SL_INDEX_SPECIFIED     ? 1 : 0;
	int flag_restart_scan        = IrpSp->Flags & SL_RESTART_SCAN        ? 1 : 0;
	int flag_return_single_entry = IrpSp->Flags & SL_RETURN_SINGLE_ENTRY ? 1 : 0;
	int bytes_out = 0;
	int index = 0;
	uio_t *uio;
	int eof = 0;
	int numdirent;
	int ret;
	mount_t *zmo;
	zfsvfs_t *zfsvfs;
	NTSTATUS Status = STATUS_NO_SUCH_FILE;

	if ((Irp->UserBuffer == NULL && Irp->MdlAddress == NULL) ||
		IrpSp->Parameters.QueryDirectory.Length <= 0)
		return STATUS_INSUFFICIENT_RESOURCES;

	if (IrpSp->FileObject == NULL || 
		IrpSp->FileObject->FsContext == NULL ||  // vnode
		IrpSp->FileObject->FsContext2 == NULL)   // ccb
		return STATUS_INVALID_PARAMETER;

	struct vnode *dvp = IrpSp->FileObject->FsContext;
	zfs_dirlist_t *zccb = IrpSp->FileObject->FsContext2;

	if (zccb->magic != ZFS_DIRLIST_MAGIC)
		return STATUS_INVALID_PARAMETER;

	// Restarting listing? Clear EOF
	if (flag_restart_scan) {
		zccb->dir_eof = 0;
		zccb->uio_offset = 0;
	}

	// Did last call complete listing?
	if (zccb->dir_eof)
		return STATUS_NO_MORE_FILES;

	uio = uio_create(1, 0, UIO_SYSSPACE, UIO_READ);	

	if (Irp->MdlAddress)
		uio_addiov(uio, MmGetSystemAddressForMdl(Irp->MdlAddress), IrpSp->Parameters.QueryDirectory.Length);
	else
		uio_addiov(uio, Irp->UserBuffer, IrpSp->Parameters.QueryDirectory.Length);

	uio_setoffset(uio, zccb->uio_offset);

	// Grab the root zp
	zmo = DeviceObject->DeviceExtension;
	ASSERT(zmo->type == MOUNT_TYPE_VCB);

	zfsvfs = vfs_fsprivate(zmo); // or from zp

	if (!zfsvfs) return STATUS_INTERNAL_ERROR;

	dprintf("%s: starting vp %p Search pattern '%wZ'\n", __func__, dvp,
		IrpSp->Parameters.QueryDirectory.FileName);

	VN_HOLD(dvp);
	ret = zfs_readdir(dvp, uio, NULL, &zccb->dir_eof, IrpSp->Flags, &numdirent);
	VN_RELE(dvp);

	if (ret == 0) {
		// Remember directory index for next time
		zccb->uio_offset = uio_offset(uio);

		// Set correct buffer size returned.
		Irp->IoStatus.Information = IrpSp->Parameters.QueryDirectory.Length - uio_resid(uio);

		// Return saying there are entries in buffer
		Status = STATUS_SUCCESS;
	}

	// Release uio
	uio_free(uio);

	return Status;
}


NTSTATUS query_directory(PDEVICE_OBJECT DeviceObject, PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
	NTSTATUS Status = STATUS_NOT_IMPLEMENTED;

	switch (IrpSp->Parameters.QueryDirectory.FileInformationClass) {

	case FileBothDirectoryInformation:
		dprintf("   %s FileBothDirectoryInformation\n", __func__);
		break;
	case FileDirectoryInformation:
		dprintf("   %s FileDirectoryInformation\n", __func__);
		break;
	case FileFullDirectoryInformation: // ***
		dprintf("   %s FileFullDirectoryInformation\n", __func__);
		// Status = STATUS_NO_MORE_FILES;
		// NextEntry += (ULONG)QuadAlign(BaseLength + BytesConverted);
		Status = query_directory_FileFullDirectoryInformation(DeviceObject, Irp, IrpSp);
		break;
	case FileIdBothDirectoryInformation:
		dprintf("   %s FileIdBothDirectoryInformation\n", __func__);
		break;
	case FileIdFullDirectoryInformation:
		dprintf("   %s FileIdFullDirectoryInformation\n", __func__);
		break;
	case FileNamesInformation:
		dprintf("   %s FileNamesInformation\n", __func__);
		break;
	case FileObjectIdInformation:
		dprintf("   %s FileObjectIdInformation\n", __func__);
		break;
	case FileQuotaInformation:
		dprintf("   %s FileQuotaInformation\n", __func__);
		break;
	case FileReparsePointInformation:
		dprintf("   %s FileReparsePointInformation\n", __func__);
		break;
	default:
		dprintf("   %s unkown 0x%x\n", __func__, IrpSp->Parameters.QueryDirectory.FileInformationClass);
		break;
	}

	return Status;
}


NTSTATUS fs_read(PDEVICE_OBJECT DeviceObject, PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
	PFILE_OBJECT	fileObject;
	ULONG			bufferLength;
	LARGE_INTEGER	byteOffset;

	dprintf("   %s\n", __func__);
	
	fileObject = IrpSp->FileObject;

	if (fileObject == NULL) {
		dprintf("  fileObject == NULL\n");
		return STATUS_INVALID_PARAMETER;
	}

	bufferLength = IrpSp->Parameters.Read.Length;
	if (IrpSp->Parameters.Read.ByteOffset.LowPart == FILE_USE_FILE_POINTER_POSITION &&
		IrpSp->Parameters.Read.ByteOffset.HighPart == -1) {
		byteOffset = fileObject->CurrentByteOffset;
	} else {
		byteOffset = IrpSp->Parameters.Read.ByteOffset;
	}

	dprintf("  FileName: %wZ offset 0x%llx len 0x%lx\n", &fileObject->FileName,
		byteOffset.QuadPart, bufferLength);

	if (bufferLength == 0) {
		Irp->IoStatus.Information = 0; // Bytes read
		return STATUS_SUCCESS;
	}

	Irp->IoStatus.Information = 0; // Bytes read
	return STATUS_SUCCESS;
}



//#define IOCTL_VOLUME_BASE ((DWORD) 'V')
//#define IOCTL_VOLUME_GET_GPT_ATTRIBUTES      CTL_CODE(IOCTL_VOLUME_BASE,14,METHOD_BUFFERED,FILE_ANY_ACCESS)

#define IOCTL_VOLUME_POST_ONLINE    CTL_CODE(IOCTL_VOLUME_BASE, 25, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)



_Function_class_(DRIVER_DISPATCH)
static NTSTATUS
ioctlDispatcher(
	_In_ PDEVICE_OBJECT DeviceObject,
	_Inout_ PIRP Irp,
	PIO_STACK_LOCATION IrpSp
)
{
	NTSTATUS Status;

	PAGED_CODE();

	dprintf("  %s: enter: major %d: minor %d: %s ioctlDeviceObject\n", __func__, IrpSp->MajorFunction, IrpSp->MinorFunction,
		major2str(IrpSp->MajorFunction, IrpSp->MinorFunction));


	Status = STATUS_NOT_IMPLEMENTED;

	switch (IrpSp->MajorFunction) {

	case IRP_MJ_CREATE:
		dprintf("IRP_MJ_CREATE: FileObject %p related %p name '%wZ' flags 0x%x\n",
			IrpSp->FileObject, IrpSp->FileObject ? IrpSp->FileObject->RelatedFileObject : NULL,
			IrpSp->FileObject->FileName, IrpSp->Flags);
		Status = zfsdev_open(DeviceObject, Irp);
		break;
	case IRP_MJ_CLOSE:
		Status = zfsdev_release(DeviceObject, Irp);
		break;
	case IRP_MJ_DEVICE_CONTROL:
		{
			/* Is it a ZFS ioctl? */
			u_long cmd = IrpSp->Parameters.DeviceIoControl.IoControlCode;
			if (cmd >= ZFS_IOC_FIRST &&
				cmd < ZFS_IOC_LAST) {
				Status = zfsdev_ioctl(DeviceObject, Irp);
				break;
			}
			/* Not ZFS ioctl, handle Windows ones */
			switch (cmd) {
			case IOCTL_VOLUME_GET_GPT_ATTRIBUTES:
				dprintf("IOCTL_VOLUME_GET_GPT_ATTRIBUTES\n");
				Status = 0;
				break;
			case IOCTL_MOUNTDEV_QUERY_DEVICE_NAME:
				dprintf("IOCTL_MOUNTDEV_QUERY_DEVICE_NAME\n");
				Status = ioctl_query_device_name(DeviceObject, Irp, IrpSp);
				break;
			case IOCTL_MOUNTDEV_QUERY_UNIQUE_ID:
				dprintf("IOCTL_MOUNTDEV_QUERY_UNIQUE_ID\n");
				Status = ioctl_query_unique_id(DeviceObject, Irp, IrpSp);
				break;
			case IOCTL_MOUNTDEV_QUERY_STABLE_GUID:
				dprintf("IOCTL_MOUNTDEV_QUERY_STABLE_GUID\n");
				break;
			case IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_NAME:
				dprintf("IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_NAME\n");
				break;
			case IOCTL_VOLUME_ONLINE:
				dprintf("IOCTL_VOLUME_ONLINE\n");
				Status = STATUS_SUCCESS;
				break;
			case IOCTL_DISK_IS_WRITABLE:
				dprintf("IOCTL_DISK_IS_WRITABLE\n");
				Status = STATUS_SUCCESS;
				break;
			case IOCTL_DISK_MEDIA_REMOVAL:
				dprintf("IOCTL_DISK_MEDIA_REMOVAL\n");
				Status = STATUS_SUCCESS;
				break;
			case IOCTL_STORAGE_MEDIA_REMOVAL:
				dprintf("IOCTL_STORAGE_MEDIA_REMOVAL\n");
				Status = STATUS_SUCCESS;
				break;
			case IOCTL_VOLUME_POST_ONLINE:
				dprintf("IOCTL_VOLUME_POST_ONLINE\n");
				Status = STATUS_SUCCESS;
				break;
			default:
				dprintf("**** unknown Windows IOCTL: 0x%lx\n", cmd);
			}

		}
		break;

	case IRP_MJ_CLEANUP:
		Status = STATUS_SUCCESS;
		break;

	case IRP_MJ_FILE_SYSTEM_CONTROL:
		switch (IrpSp->MinorFunction) {
		case IRP_MN_MOUNT_VOLUME:
			Status = zfs_vnop_mount(Irp, IrpSp);
			break;
		}
		break;

	case IRP_MJ_PNP:
		switch (IrpSp->MinorFunction) {
		case IRP_MN_QUERY_CAPABILITIES:
			Status = QueryCapabilities(DeviceObject, Irp, IrpSp);
			break;
		case IRP_MN_QUERY_DEVICE_RELATIONS:
			Status = STATUS_NOT_IMPLEMENTED;
			break;
		case IRP_MN_QUERY_ID:
			Status = pnp_query_id(DeviceObject, Irp, IrpSp);
			break;
		case IRP_MN_QUERY_PNP_DEVICE_STATE:
			Status = pnp_device_state(DeviceObject, Irp, IrpSp);
			break;
		case IRP_MN_QUERY_REMOVE_DEVICE:
			dprintf("IRP_MN_QUERY_REMOVE_DEVICE\n");
			Status = STATUS_SUCCESS;
			break;
		case IRP_MN_SURPRISE_REMOVAL:
			dprintf("IRP_MN_SURPRISE_REMOVAL\n");
			Status = STATUS_SUCCESS;
			break;
		case IRP_MN_REMOVE_DEVICE:
			dprintf("IRP_MN_REMOVE_DEVICE\n");
			Status = STATUS_SUCCESS;
			break;
		case IRP_MN_CANCEL_REMOVE_DEVICE:
			dprintf("IRP_MN_CANCEL_REMOVE_DEVICE\n");
			Status = STATUS_SUCCESS;
			break;
		}
		break;

	}

	return Status;
}


_Function_class_(DRIVER_DISPATCH)
static NTSTATUS
diskDispatcher(
	_In_ PDEVICE_OBJECT DeviceObject,
	_Inout_ PIRP Irp,
	PIO_STACK_LOCATION IrpSp
)
{
	NTSTATUS Status;

	PAGED_CODE();

	dprintf("  %s: enter: major %d: minor %d: %s diskDeviceObject\n", __func__, IrpSp->MajorFunction, IrpSp->MinorFunction,
		major2str(IrpSp->MajorFunction, IrpSp->MinorFunction));


	Status = STATUS_NOT_IMPLEMENTED;

	switch (IrpSp->MajorFunction) {

	case IRP_MJ_CREATE:
		dprintf("IRP_MJ_CREATE: FileObject %p related %p name '%wZ' flags 0x%x\n",
			IrpSp->FileObject, IrpSp->FileObject ? IrpSp->FileObject->RelatedFileObject : NULL,
			IrpSp->FileObject->FileName, IrpSp->Flags);
		Status = STATUS_SUCCESS;

		mount_t *zmo = DeviceObject->DeviceExtension;
		VERIFY(zmo->type == MOUNT_TYPE_DCB);

		if (zmo->deviceObject != NULL)
			IrpSp->FileObject->Vpb = zmo->deviceObject->Vpb;
		else
			IrpSp->FileObject->Vpb = DeviceObject->Vpb;
		dprintf("Setting FileObject->Vpb to %p\n", IrpSp->FileObject->Vpb);
		if (IrpSp->Parameters.Create.Options & FILE_DIRECTORY_FILE) {
			Status = STATUS_NOT_A_DIRECTORY;
		} else {
			//SetFileObjectForVCB(IrpSp->FileObject, zmo);
			//IrpSp->FileObject->SectionObjectPointer = &zmo->SectionObjectPointers;
			//IrpSp->FileObject->FsContext = &zmo->VolumeFileHeader;
			Irp->IoStatus.Information = FILE_OPENED;
			Status = STATUS_SUCCESS;
		}
		break;
	case IRP_MJ_CLOSE:
		Status = STATUS_SUCCESS;
		break;
	case IRP_MJ_DEVICE_CONTROL:
	{
		u_long cmd = IrpSp->Parameters.DeviceIoControl.IoControlCode;
		/* Not ZFS ioctl, handle Windows ones */
		switch (cmd) {
		case IOCTL_VOLUME_GET_GPT_ATTRIBUTES:
			dprintf("IOCTL_VOLUME_GET_GPT_ATTRIBUTES\n");
			Status = 0;
			break;
		case IOCTL_MOUNTDEV_QUERY_DEVICE_NAME:
			dprintf("IOCTL_MOUNTDEV_QUERY_DEVICE_NAME\n");
			Status = ioctl_query_device_name(DeviceObject, Irp, IrpSp);
			break;
		case IOCTL_MOUNTDEV_QUERY_UNIQUE_ID:
			dprintf("IOCTL_MOUNTDEV_QUERY_UNIQUE_ID\n");
			Status = ioctl_query_unique_id(DeviceObject, Irp, IrpSp);
			break;
		case IOCTL_MOUNTDEV_QUERY_STABLE_GUID:
			dprintf("IOCTL_MOUNTDEV_QUERY_STABLE_GUID\n");
			break;
		case IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_NAME:
			dprintf("IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_NAME\n");
			break;
		case IOCTL_VOLUME_ONLINE:
			dprintf("IOCTL_VOLUME_ONLINE\n");
			Status = STATUS_SUCCESS;
			break;
		case IOCTL_DISK_IS_WRITABLE:
			dprintf("IOCTL_DISK_IS_WRITABLE\n");
			Status = STATUS_SUCCESS;
			break;
		case IOCTL_DISK_MEDIA_REMOVAL:
			dprintf("IOCTL_DISK_MEDIA_REMOVAL\n");
			Status = STATUS_SUCCESS;
			break;
		case IOCTL_STORAGE_MEDIA_REMOVAL:
			dprintf("IOCTL_STORAGE_MEDIA_REMOVAL\n");
			Status = STATUS_SUCCESS;
			break;
		case IOCTL_VOLUME_POST_ONLINE:
			dprintf("IOCTL_VOLUME_POST_ONLINE\n");
			Status = STATUS_SUCCESS;
			break;
		case IOCTL_VOLUME_IS_DYNAMIC:
		{
			uint8_t *buf = (UINT8*)Irp->AssociatedIrp.SystemBuffer;
			*buf = 1;
			Irp->IoStatus.Information = 1;
			Status = STATUS_SUCCESS;
			break;
		}
		case IOCTL_MOUNTDEV_LINK_CREATED:
			dprintf("IOCTL_MOUNTDEV_LINK_CREATED\n");
			Status = STATUS_SUCCESS;
			break;
		case 0x4d0010: // Same as IOCTL_MOUNTDEV_LINK_CREATED but bit 14,15 are 0 (access permissions)
			dprintf("IOCTL_MOUNTDEV_LINK_CREATED v2\n");
			Status = STATUS_SUCCESS;
			break;

		default:
			dprintf("**** unknown Windows IOCTL: 0x%lx\n", cmd);
		}

	}
	break;

	case IRP_MJ_CLEANUP:
		Status = 0;
		break;

	case IRP_MJ_FILE_SYSTEM_CONTROL:
		switch (IrpSp->MinorFunction) {
		case IRP_MN_MOUNT_VOLUME:
			Status = zfs_vnop_mount(Irp, IrpSp);
			break;
		}
		break;

	case IRP_MJ_PNP:
		switch (IrpSp->MinorFunction) {
		case IRP_MN_QUERY_CAPABILITIES:
			Status = QueryCapabilities(DeviceObject, Irp, IrpSp);
			break;
		case IRP_MN_QUERY_DEVICE_RELATIONS:
			Status = STATUS_NOT_IMPLEMENTED;
			break;
		case IRP_MN_QUERY_ID:
			Status = pnp_query_id(DeviceObject, Irp, IrpSp);
			break;
		case IRP_MN_QUERY_PNP_DEVICE_STATE:
			Status = pnp_device_state(DeviceObject, Irp, IrpSp);
			break;
		case IRP_MN_QUERY_REMOVE_DEVICE:
			dprintf("IRP_MN_QUERY_REMOVE_DEVICE\n");
			Status = STATUS_SUCCESS;
			break;
		case IRP_MN_SURPRISE_REMOVAL:
			dprintf("IRP_MN_SURPRISE_REMOVAL\n");
			Status = STATUS_SUCCESS;
			break;
		case IRP_MN_REMOVE_DEVICE:
			dprintf("IRP_MN_REMOVE_DEVICE\n");
			Status = STATUS_SUCCESS;
			break;
		case IRP_MN_CANCEL_REMOVE_DEVICE:
			dprintf("IRP_MN_CANCEL_REMOVE_DEVICE\n");
			Status = STATUS_SUCCESS;
			break;
		}
		break;

	}

	return Status;
}



_Function_class_(DRIVER_DISPATCH)
static NTSTATUS
fsDispatcher(
	_In_ PDEVICE_OBJECT DeviceObject,
	_Inout_ PIRP Irp,
	PIO_STACK_LOCATION IrpSp
)
{
	NTSTATUS Status;

	PAGED_CODE();

	dprintf("  %s: enter: major %d: minor %d: %s fsDeviceObject\n", __func__, IrpSp->MajorFunction, IrpSp->MinorFunction,
		major2str(IrpSp->MajorFunction, IrpSp->MinorFunction));


	Status = STATUS_NOT_IMPLEMENTED;

	switch (IrpSp->MajorFunction) {

	case IRP_MJ_CREATE:
		dprintf("IRP_MJ_CREATE: FileObject %p related %p name '%wZ' flags 0x%x\n",
			IrpSp->FileObject, IrpSp->FileObject ? IrpSp->FileObject->RelatedFileObject : NULL,
			IrpSp->FileObject->FileName, IrpSp->Flags);

		Irp->IoStatus.Information = FILE_OPENED;
		Status = STATUS_SUCCESS;

		// Disallow autorun.inf for now
		if (IrpSp && IrpSp->FileObject && IrpSp->FileObject->FileName.Buffer &&
			_wcsicmp(IrpSp->FileObject->FileName.Buffer, L"\\autorun.inf") == 0) {
			Irp->IoStatus.Information = FILE_DOES_NOT_EXIST;
			Status = STATUS_OBJECT_PATH_NOT_FOUND;
			break;
		}


		mount_t *zmo = DeviceObject->DeviceExtension;
		VERIFY(zmo->type == MOUNT_TYPE_VCB);

		if (zmo->deviceObject != NULL)
			IrpSp->FileObject->Vpb = zmo->deviceObject->Vpb;
		else
			IrpSp->FileObject->Vpb = DeviceObject->Vpb;
		dprintf("Setting FileObject->Vpb to %p\n", IrpSp->FileObject->Vpb);


		//
		//  Check if we are opening the volume and not a file/directory.
		//  We are opening the volume if the name is empty and there
		//  isn't a related file object.  If there is a related file object
		//  then it is the Vcb itself.
		//
		if (IrpSp->FileObject->FileName.Length == 0) {
			// relatedFileObject should be NULL, OR, point to UserVolumeOpen
			// This opens the Volume; we should handle reading of it too

			// If DirectoryFile return STATUS_NOT_A_DIRECTORY
			// If OpenTargetDirectory return STATUS_INVALID_PARAMETER

			break;
		}

		// We have a name, so we are looking for something specific
		// Attempt to find the requested object
		if (IrpSp && IrpSp->FileObject && IrpSp->FileObject->FileName.Buffer &&
			zmo) {

			Status = zfs_vnop_lookup(Irp, IrpSp, zmo);

		}
		break;

	case IRP_MJ_CLOSE:
		Status = STATUS_SUCCESS;
		if (IrpSp->FileObject && IrpSp->FileObject->FsContext2) {
			zfs_dirlist_t *zccb = IrpSp->FileObject->FsContext2;
			if (zccb->magic == ZFS_DIRLIST_MAGIC) {
				kmem_free(IrpSp->FileObject->FsContext2, sizeof(zfs_dirlist_t));
				IrpSp->FileObject->FsContext2 = NULL;
			}
		}
		break;
	case IRP_MJ_DEVICE_CONTROL:
	{
		u_long cmd = IrpSp->Parameters.DeviceIoControl.IoControlCode;
		/* Not ZFS ioctl, handle Windows ones */
		switch (cmd) {
		case IOCTL_VOLUME_GET_GPT_ATTRIBUTES:
			dprintf("IOCTL_VOLUME_GET_GPT_ATTRIBUTES\n");
			Status = 0;
			break;
		case IOCTL_MOUNTDEV_QUERY_DEVICE_NAME:
			dprintf("IOCTL_MOUNTDEV_QUERY_DEVICE_NAME\n");
			Status = ioctl_query_device_name(DeviceObject, Irp, IrpSp);
			break;
		case IOCTL_MOUNTDEV_QUERY_UNIQUE_ID:
			dprintf("IOCTL_MOUNTDEV_QUERY_UNIQUE_ID\n");
			Status = ioctl_query_unique_id(DeviceObject, Irp, IrpSp);
			break;
		case IOCTL_MOUNTDEV_QUERY_STABLE_GUID:
			dprintf("IOCTL_MOUNTDEV_QUERY_STABLE_GUID\n");
			break;
		case IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_NAME:
			dprintf("IOCTL_MOUNTDEV_QUERY_SUGGESTED_LINK_NAME\n");
			break;
		case IOCTL_VOLUME_ONLINE:
			dprintf("IOCTL_VOLUME_ONLINE\n");
			Status = STATUS_SUCCESS;
			break;
		case IOCTL_DISK_IS_WRITABLE:
			dprintf("IOCTL_DISK_IS_WRITABLE\n");
			Status = STATUS_SUCCESS;
			break;
		case IOCTL_DISK_MEDIA_REMOVAL:
			dprintf("IOCTL_DISK_MEDIA_REMOVAL\n");
			Status = STATUS_SUCCESS;
			break;
		case IOCTL_STORAGE_MEDIA_REMOVAL:
			dprintf("IOCTL_STORAGE_MEDIA_REMOVAL\n");
			Status = STATUS_SUCCESS;
			break;
		case IOCTL_VOLUME_POST_ONLINE:
			dprintf("IOCTL_VOLUME_POST_ONLINE\n");
			Status = STATUS_SUCCESS;
			break;
		case IOCTL_STORAGE_CHECK_VERIFY:
			dprintf("IOCTL_STORAGE_CHECK_VERIFY\n");
			Status = STATUS_SUCCESS;
			break;
		default:
			dprintf("**** unknown Windows IOCTL: 0x%lx\n", cmd);
		}

	}
	break;

	case IRP_MJ_CLEANUP:
#if 0 // Figure out how this works
		if (IrpSp->FileObject && IrpSp->FileObject->FsContext) {
			struct vnode *vp = IrpSp->FileObject->FsContext;
			znode_t *zp = VTOZ(vp);
			zfsvfs_t *zfsvfs = zp->z_zfsvfs;
			DbgBreakPoint();
			// Decouple the nodes
			ZTOV(zp) = NULL;
			vnode_clearfsnode(vp); /* vp->v_data = NULL */

			dprintf("IRP_MJ_CLEANUP: releasing zp %p and vp %p\n", zp, vp);

			// Release znode
			rw_enter(&zfsvfs->z_teardown_inactive_lock, RW_READER);
			if (zp->z_sa_hdl == NULL)
				zfs_znode_free(zp);
			else
				zfs_zinactive(zp);
			rw_exit(&zfsvfs->z_teardown_inactive_lock);

			// Free vp memory
			kmem_free(vp, sizeof(*vp));

			atomic_inc_64(&vnop_num_reclaims);
		}
#endif
		Status = STATUS_SUCCESS;
		break;

	case IRP_MJ_FILE_SYSTEM_CONTROL:
		switch (IrpSp->MinorFunction) {
		case IRP_MN_MOUNT_VOLUME:
			Status = zfs_vnop_mount(Irp, IrpSp);
			break;
		case IRP_MN_USER_FS_REQUEST:
			Status = user_fs_request(DeviceObject, Irp, IrpSp);
			break;
		}
		break;

	case IRP_MJ_PNP:
		switch (IrpSp->MinorFunction) {
		case IRP_MN_QUERY_CAPABILITIES:
			Status = QueryCapabilities(DeviceObject, Irp, IrpSp);
			break;
		case IRP_MN_QUERY_DEVICE_RELATIONS:
			Status = STATUS_NOT_IMPLEMENTED;
			break;
		case IRP_MN_QUERY_ID:
			Status = pnp_query_id(DeviceObject, Irp, IrpSp);
			break;
		case IRP_MN_QUERY_PNP_DEVICE_STATE:
			Status = pnp_device_state(DeviceObject, Irp, IrpSp);
			break;
		case IRP_MN_QUERY_REMOVE_DEVICE:
			dprintf("IRP_MN_QUERY_REMOVE_DEVICE\n");
			Status = STATUS_SUCCESS;
			break;
		case IRP_MN_SURPRISE_REMOVAL:
			dprintf("IRP_MN_SURPRISE_REMOVAL\n");
			Status = STATUS_SUCCESS;
			break;
		case IRP_MN_REMOVE_DEVICE:
			dprintf("IRP_MN_REMOVE_DEVICE\n");
			Status = STATUS_SUCCESS;
			break;
		case IRP_MN_CANCEL_REMOVE_DEVICE:
			dprintf("IRP_MN_CANCEL_REMOVE_DEVICE\n");
			Status = STATUS_SUCCESS;
			break;
		}
		break;
	case IRP_MJ_QUERY_VOLUME_INFORMATION:
		Status = query_volume_information(DeviceObject, Irp, IrpSp);
		break;

	case IRP_MJ_LOCK_CONTROL:
		Status = lock_control(DeviceObject, Irp, IrpSp);
		break;

	case IRP_MJ_QUERY_INFORMATION:
		Status = query_information(DeviceObject, Irp, IrpSp);
		break;

	case IRP_MJ_DIRECTORY_CONTROL:
		switch (IrpSp->MinorFunction) {
		case IRP_MN_NOTIFY_CHANGE_DIRECTORY:
			break;
		case IRP_MN_QUERY_DIRECTORY:
			Status = query_directory(DeviceObject, Irp, IrpSp);
			break;
		}
		break;

	case IRP_MJ_READ:
		Status = fs_read(DeviceObject, Irp, IrpSp);
		break;
	}

	return Status;
}


_Function_class_(DRIVER_DISPATCH)
static NTSTATUS
dispatcher(
	_In_ PDEVICE_OBJECT DeviceObject,
	_Inout_ PIRP Irp
)
{
	BOOLEAN TopLevel = FALSE;
	PIO_STACK_LOCATION IrpSp;
	NTSTATUS Status;

	PAGED_CODE();

	//dprintf("%s: enter\n", __func__);

	//  If we were called with our file system device object instead of a
	//  volume device object, just complete this request with STATUS_SUCCESS
#if 0
	if (vnop_deviceObject == VolumeDeviceObject) {
		dprintf("%s: own object\n", __func__);
		Irp->IoStatus.Status = STATUS_SUCCESS;
		Irp->IoStatus.Information = FILE_OPENED;
		IoCompleteRequest(Irp, IO_DISK_INCREMENT);
		return STATUS_SUCCESS;
	}
#endif

	FsRtlEnterFileSystem();

	if (IoGetTopLevelIrp() == NULL) {
		IoSetTopLevelIrp(Irp);
		TopLevel = TRUE;
	}

	IrpSp = IoGetCurrentIrpStackLocation(Irp);
	dprintf("%s: enter: major %d: minor %d: %s\n", __func__, IrpSp->MajorFunction, IrpSp->MinorFunction,
		major2str(IrpSp->MajorFunction, IrpSp->MinorFunction));

	Status = STATUS_NOT_IMPLEMENTED;

	if (DeviceObject == ioctlDeviceObject)
		Status = ioctlDispatcher(DeviceObject, Irp, IrpSp);
	else if (DeviceObject == diskDeviceObject)
		Status = diskDispatcher(DeviceObject, Irp, IrpSp);
	else if (DeviceObject == fsDeviceObject)
		Status = fsDispatcher(DeviceObject, Irp, IrpSp);
	
	Irp->IoStatus.Status = Status;

	if (TopLevel) { IoSetTopLevelIrp(NULL); }
	FsRtlExitFileSystem();

	dprintf("%s: exit: 0x%x Information 0x%x\n", __func__, Status, Irp->IoStatus.Information);
	IoCompleteRequest(Irp, Status == STATUS_SUCCESS ? IO_DISK_INCREMENT : IO_NO_INCREMENT);
	return Status;
}

void zfs_windows_vnops_callback(PDEVICE_OBJECT deviceObject)
{
	WIN_DriverObject->MajorFunction[IRP_MJ_CREATE] = (PDRIVER_DISPATCH)dispatcher;   // zfs_ioctl.c
	WIN_DriverObject->MajorFunction[IRP_MJ_CLOSE] = (PDRIVER_DISPATCH)dispatcher;     // zfs_ioctl.c
	WIN_DriverObject->MajorFunction[IRP_MJ_READ] = (PDRIVER_DISPATCH)dispatcher;
	WIN_DriverObject->MajorFunction[IRP_MJ_WRITE] = (PDRIVER_DISPATCH)dispatcher;
	WIN_DriverObject->MajorFunction[IRP_MJ_QUERY_INFORMATION] = (PDRIVER_DISPATCH)dispatcher;
	WIN_DriverObject->MajorFunction[IRP_MJ_SET_INFORMATION] = (PDRIVER_DISPATCH)dispatcher;
	WIN_DriverObject->MajorFunction[IRP_MJ_QUERY_EA] = (PDRIVER_DISPATCH)dispatcher;
	WIN_DriverObject->MajorFunction[IRP_MJ_SET_EA] = (PDRIVER_DISPATCH)dispatcher;
	WIN_DriverObject->MajorFunction[IRP_MJ_FLUSH_BUFFERS] = (PDRIVER_DISPATCH)dispatcher;
	WIN_DriverObject->MajorFunction[IRP_MJ_QUERY_VOLUME_INFORMATION] = (PDRIVER_DISPATCH)dispatcher;
	WIN_DriverObject->MajorFunction[IRP_MJ_SET_VOLUME_INFORMATION] = (PDRIVER_DISPATCH)dispatcher;
	WIN_DriverObject->MajorFunction[IRP_MJ_DIRECTORY_CONTROL] = (PDRIVER_DISPATCH)dispatcher;
	WIN_DriverObject->MajorFunction[IRP_MJ_FILE_SYSTEM_CONTROL] = (PDRIVER_DISPATCH)dispatcher;
	WIN_DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = (PDRIVER_DISPATCH)dispatcher; // zfs_ioctl.c
	WIN_DriverObject->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = (PDRIVER_DISPATCH)dispatcher; // zfs_ioctl.c
	WIN_DriverObject->MajorFunction[IRP_MJ_SHUTDOWN] = (PDRIVER_DISPATCH)dispatcher;
	WIN_DriverObject->MajorFunction[IRP_MJ_LOCK_CONTROL] = (PDRIVER_DISPATCH)dispatcher;
	WIN_DriverObject->MajorFunction[IRP_MJ_CLEANUP] = (PDRIVER_DISPATCH)dispatcher;
	WIN_DriverObject->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = (PDRIVER_DISPATCH)dispatcher;
	WIN_DriverObject->MajorFunction[IRP_MJ_DEVICE_CHANGE] = (PDRIVER_DISPATCH)dispatcher;
	WIN_DriverObject->MajorFunction[IRP_MJ_PNP] = (PDRIVER_DISPATCH)dispatcher;

#if 0
	RtlZeroMemory(&FilterCallbacks,
		sizeof(FS_FILTER_CALLBACKS));

	FilterCallbacks.SizeOfFsFilterCallbacks = sizeof(FS_FILTER_CALLBACKS);
	FilterCallbacks.PreAcquireForSectionSynchronization = FatFilterCallbackAcquireForCreateSection;

	Status = FsRtlRegisterFileSystemFilterCallbacks(DriverObject,
		&FilterCallbacks);
#endif

}


int
zfs_vfsops_init(void)
{
	zfs_init();
	return 0;
}

int
zfs_vfsops_fini(void)
{
	zfs_fini();
	return 0;
}
