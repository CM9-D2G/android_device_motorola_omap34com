/**********************************************************************
 *
 * Copyright (C) Imagination Technologies Ltd. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful but, except
 * as otherwise stated in writing, without any warranty; without even the
 * implied warranty of merchantability or fitness for a particular purpose.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 * Contact Information:
 * Imagination Technologies Ltd. <gpl-support@imgtec.com>
 * Home Park Estate, Kings Langley, Herts, WD4 8LZ, UK
 *
 *****************************************************************************/

#include "ion.h"

#include "syscommon.h"
#include "mutex.h"
#include "lock.h"
#include "mm.h"
#include "perproc.h"
#include "env_perproc.h"
#include "private_data.h"
#include "pvr_debug.h"
#include "pvr_bridge_km.h"
#include "proc.h"
#include "sgxfeaturedefs.h"
#include "sgxmmu.h"
#include "sgxinfokm.h"
#include <linux/module.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/seq_file.h>

//#include "../../misc/symsearch/symsearch.h"
#include "../symsearch/symsearch.h"

SYMSEARCH_DECLARE_FUNCTION_STATIC(PVRSRV_ERROR, _PVRSRVLookupHandle, PVRSRV_HANDLE_BASE *psBase, IMG_PVOID *ppvData, IMG_HANDLE hHandle, PVRSRV_HANDLE_TYPE eType);

SYMSEARCH_DECLARE_FUNCTION_STATIC(PVRSRV_ERROR, _PVRSRVPerProcessDataConnect, IMG_UINT32 ui32PID);
SYMSEARCH_DECLARE_FUNCTION_STATIC(IMG_VOID,     _PVRSRVPerProcessDataDisconnect, IMG_UINT32  ui32PID);
SYMSEARCH_DECLARE_FUNCTION_STATIC(PVRSRV_PER_PROCESS_DATA *, _PVRSRVPerProcessData, IMG_UINT32 ui32PID);
SYMSEARCH_DECLARE_FUNCTION_STATIC(PVRSRV_ERROR, _PVRSRVPerProcessDataInit, IMG_VOID);
SYMSEARCH_DECLARE_FUNCTION_STATIC(PVRSRV_ERROR, _PVRSRVPerProcessDataDeInit, IMG_VOID);

#include "hook.h"

static struct mutex mlock;
#define LinuxLockMutex(m) mutex_lock(&mlock)
#define LinuxUnLockMutex(m) mutex_unlock(&mlock)

// this key pointers are filled by the hooked function
static SYS_DATA *psSysData;
static PVRSRV_SGXDEV_INFO *psSgxDevInfo = NULL;
// not sure how to get it... using psPerProc->psHandleBase
static PVRSRV_HANDLE_BASE *psKernelHandleBase = NULL;

static bool hooked = false;
static bool connected = false;
static bool datainit = false;

struct ion_client *gpsIONClient;
EXPORT_SYMBOL(gpsIONClient);

struct ion_handle *
PVRSRVExportFDToIONHandle(int fd, struct ion_client **client)
{
	struct ion_handle *psIONHandle = IMG_NULL;
	PVRSRV_FILE_PRIVATE_DATA *psPrivateData;
	PVRSRV_KERNEL_MEM_INFO *psKernelMemInfo;
	LinuxMemArea *psLinuxMemArea;
	PVRSRV_ERROR eError = PVRSRV_OK;
	struct file *psFile;

	if(!psKernelHandleBase) {
		printk(KERN_ERR "PVR base handle not found yet ! 'cat /proc/pvr/nodes' to force the hook...\n");
		return psIONHandle;
	}

	/* Take the bridge mutex so the handle won't be freed underneath us */
	LinuxLockMutex(&gPVRSRVLock);

	psFile = fget(fd);
	if(!psFile)
		goto err_unlock;

	psPrivateData = psFile->private_data;
	if(!psPrivateData)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: struct file* has no private_data; "
								"invalid export handle", __func__));
		goto err_fput;
	}

	SYMSEARCH_BIND_FUNCTION_TO_TYPED(ionpvr, struct ion_handle *, PVRSRVLookupHandle, _PVRSRVLookupHandle);
	eError = _PVRSRVLookupHandle(psKernelHandleBase,
								(IMG_PVOID *)&psKernelMemInfo,
								psPrivateData->hKernelMemInfo,
								PVRSRV_HANDLE_TYPE_MEM_INFO);
	if(eError != PVRSRV_OK && eError != (PVRSRV_ERROR) -ENXIO)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Failed to look up MEM_INFO handle",
								__func__));
		goto err_fput;
	}

	psLinuxMemArea = (LinuxMemArea *)psKernelMemInfo->sMemBlk.hOSMemHandle;
	BUG_ON(psLinuxMemArea == IMG_NULL);

	if(psLinuxMemArea->eAreaType != LINUX_MEM_AREA_ION)
	{
		PVR_DPF((PVR_DBG_ERROR, "%s: Valid handle, but not an ION buffer",
								__func__));
		goto err_fput;
	}

	psIONHandle = psLinuxMemArea->uData.sIONTilerAlloc.psIONHandle;
	//psIONHandle = sIONTilerAlloc.psIONHandle;
	if(client)
		*client = gpsIONClient;

err_fput:
	fput(psFile);
err_unlock:
	/* Allow PVRSRV clients to communicate with srvkm again */
	LinuxUnLockMutex(&gPVRSRVLock);
	return psIONHandle;
}
EXPORT_SYMBOL(PVRSRVExportFDToIONHandle);

#define INDEX_TO_HANDLE(idx) ((IMG_HANDLE)((idx) + 1))
#define HANDLE_TO_INDEX(hand) ((IMG_UINT32)(hand) - 1)
#define INDEX_IS_VALID(psBase, i) ((i) < (psBase)->ui32TotalHandCount)

static int sniff_handle_base(void) {
	IMG_UINT32 pid;
	PVRSRV_ERROR eError = PVRSRV_OK;
	PVRSRV_PER_PROCESS_DATA * psPerProc;
/*
	char * str;
	PVRSRV_MISC_INFO sMiscInfo;
	memset(&sMiscInfo, 0, sizeof(sMiscInfo));

	sMiscInfo.ui32StateRequest = PVRSRV_MISC_INFO_TIMER_PRESENT
		|PVRSRV_MISC_INFO_CLOCKGATE_PRESENT
		|PVRSRV_MISC_INFO_MEMSTATS_PRESENT
		|PVRSRV_MISC_INFO_GLOBALEVENTOBJECT_PRESENT
		|PVRSRV_MISC_INFO_DDKVERSION_PRESENT
		|PVRSRV_MISC_INFO_CPUCACHEFLUSH_PRESENT;

	SYMSEARCH_BIND_FUNCTION_TO(ionpvr, PVRSRVGetMiscInfoKM, _PVRSRVGetMiscInfoKM);
	eError = _PVRSRVGetMiscInfoKM(&sMiscInfo);

	printk(KERN_INFO "psMiscInfo:\n ui32StatePresent=%x\n hSOCTimerRegisterOSMemHandle=%p\n",
			(uint32_t) sMiscInfo.ui32StatePresent,
			sMiscInfo.hSOCTimerRegisterOSMemHandle);
	if (sMiscInfo.ui32MemoryStrLen > 0) {
		str = (char*) sMiscInfo.pszMemoryStr;
		str[sMiscInfo.ui32MemoryStrLen] = '\0';
		printk(KERN_INFO "MemoryStr(%u) = %s\n", (uint32_t) sMiscInfo.ui32MemoryStrLen, str);
	}
*/

	SYMSEARCH_BIND_FUNCTION_TO(ionpvr, PVRSRVPerProcessDataInit, _PVRSRVPerProcessDataInit);
	eError = _PVRSRVPerProcessDataInit();
	datainit = (eError == PVRSRV_OK);
	if (!datainit) {
		printk(KERN_WARNING "PVRSRVPerProcessDataInit() err = %d\n", eError);
	}

	// Trying to get gpsKernelHandleBase (or a substitute)
	SYMSEARCH_BIND_FUNCTION_TO(ionpvr, PVRSRVPerProcessDataConnect, _PVRSRVPerProcessDataConnect);
	pid = (IMG_UINT32) KERNEL_ID;//task_tgid_nr(current);
	eError = _PVRSRVPerProcessDataConnect(pid);
	if (eError == PVRSRV_OK) {
		connected = true;
		SYMSEARCH_BIND_FUNCTION_TO(ionpvr, PVRSRVPerProcessData, _PVRSRVPerProcessData);
		psPerProc = _PVRSRVPerProcessData(pid);
		printk(KERN_INFO "PVRSRVPerProcessData(0x%lx) = %p\n", pid, psPerProc);
		if (psPerProc != IMG_NULL) {
			printk(KERN_INFO " psPerProc->psHandleBase = %p\n", psPerProc->psHandleBase);
			psKernelHandleBase = psPerProc->psHandleBase;
		}
	}

	// disconnect in module exit
	//SYMSEARCH_BIND_FUNCTION_TO(ionpvr, PVRSRVPerProcessDataDisconnect, _PVRSRVPerProcessDataDisconnect);
	//_PVRSRVPerProcessDataDisconnect(KERNEL_ID);

	/*SYS_DATA            *psSysData = NULL;
	SysAcquireData(&psSysData); //need psSysData :/
	printk(KERN_INFO "psSysData = %p\n", psSysData);
	if (psSysData != NULL) {
		printk(KERN_INFO "psDeviceNodeList = %p\n", psSysData->psDeviceNodeList);
		return -ENODEV;
	}*/
	return 0;
}
/*
#define SYS_DEVICE_COUNT 3
#define SYS_MAX_LOCAL_DEVMEM_ARENAS 4

struct SYS_DATA {
	IMG_UINT32                  ui32NumDevices;
	SYS_DEVICE_ID               sDeviceID[SYS_DEVICE_COUNT];
	PVRSRV_DEVICE_NODE          *psDeviceNodeList;
	PVRSRV_POWER_DEV            *psPowerDeviceList;
	PVRSRV_RESOURCE             sPowerStateChangeResource;
	PVRSRV_SYS_POWER_STATE      eCurrentPowerState;
	PVRSRV_SYS_POWER_STATE      eFailedPowerState;
	IMG_UINT32                  ui32CurrentOSPowerState;
	PVRSRV_QUEUE_INFO           *psQueueList;
	PVRSRV_KERNEL_SYNC_INFO     *psSharedSyncInfoList;
	IMG_PVOID                   pvEnvSpecificData;
	IMG_PVOID                   pvSysSpecificData;
	PVRSRV_RESOURCE             sQProcessResource;
	IMG_VOID                    *pvSOCRegsBase;
	IMG_HANDLE                  hSOCTimerRegisterOSMemHandle;
	IMG_UINT32                  *pvSOCTimerRegisterKM;
	IMG_VOID                    *pvSOCClockGateRegsBase;
	IMG_UINT32                  ui32SOCClockGateRegsSize;
	PFN_CMD_PROC                *ppfnCmdProcList[SYS_DEVICE_COUNT];
	PCOMMAND_COMPLETE_DATA      *ppsCmdCompleteData[SYS_DEVICE_COUNT];
	IMG_BOOL                    bReProcessQueues;
	RA_ARENA                    *apsLocalDevMemArena[SYS_MAX_LOCAL_DEVMEM_ARENAS];
	IMG_CHAR                    *pszVersionString;
	PVRSRV_EVENTOBJECT          *psGlobalEventObject;
	IMG_BOOL                    bFlushAll;
}
*/
static void dump_SysData(void) {
	if (!psSysData) return;
	printk(KERN_INFO "  ui32NumDevices = %lu\n", psSysData->ui32NumDevices);
	printk(KERN_INFO "  sDeviceID[3]\n");
	printk(KERN_INFO "   uiID = %lu\n", psSysData->sDeviceID[0].uiID);
	printk(KERN_INFO "   uiID = %lu\n", psSysData->sDeviceID[1].uiID);
	printk(KERN_INFO "   uiID = %lu\n", psSysData->sDeviceID[2].uiID);
	printk(KERN_INFO "  psDeviceNodeList => %p\n", psSysData->psDeviceNodeList);
	printk(KERN_INFO "  psPowerDeviceList => %p\n", psSysData->psPowerDeviceList);
	printk(KERN_INFO "  sPowerStateChangeResource = %lu\n", psSysData->sPowerStateChangeResource.ui32ID);
	printk(KERN_INFO "  eCurrentPowerState = %d\n", psSysData->eCurrentPowerState);
	printk(KERN_INFO "  eFailedPowerState = %d\n", psSysData->eFailedPowerState);
	printk(KERN_INFO "  ui32CurrentOSPowerState = %lu\n", psSysData->ui32CurrentOSPowerState);
	printk(KERN_INFO "  psQueueList = %p\n", psSysData->psQueueList);
	printk(KERN_INFO "  psSharedSyncInfoList = %p\n", psSysData->psSharedSyncInfoList);
	printk(KERN_INFO "  pvEnvSpecificData = %p\n", psSysData->pvEnvSpecificData);
	printk(KERN_INFO "  pvSysSpecificData = %p\n", psSysData->pvSysSpecificData);
	printk(KERN_INFO "  sQProcessResource = %lu\n", psSysData->sQProcessResource.ui32ID);
	printk(KERN_INFO "  pvSOCRegsBase = %p\n", psSysData->pvSOCRegsBase);
	printk(KERN_INFO "  hSOCTimerRegisterOSMemHandle = %p\n", psSysData->hSOCTimerRegisterOSMemHandle);
	printk(KERN_INFO "  pvSOCTimerRegisterKM = %p\n", psSysData->pvSOCTimerRegisterKM);
	printk(KERN_INFO "  pvSOCClockGateRegsBase = %p\n", psSysData->pvSOCClockGateRegsBase);
	printk(KERN_INFO "  ui32SOCClockGateRegsSize = %lu\n", psSysData->ui32SOCClockGateRegsSize);
	printk(KERN_INFO "  ppfnCmdProcList[3] = %p\n", psSysData->ppfnCmdProcList);
	printk(KERN_INFO "  ppsCmdCompleteData[3] => %p\n", psSysData->ppsCmdCompleteData);
	printk(KERN_INFO "   [0] => %p\n", psSysData->ppsCmdCompleteData[0]);
	printk(KERN_INFO "   [1] => %p\n", psSysData->ppsCmdCompleteData[1]);
	printk(KERN_INFO "   [2] => %p\n", psSysData->ppsCmdCompleteData[2]);
	printk(KERN_INFO "  bReProcessQueues = %d\n", psSysData->bReProcessQueues);
	printk(KERN_INFO "  apsLocalDevMemArena[4] => %p\n", psSysData->apsLocalDevMemArena);
	printk(KERN_INFO "   [0] => %p\n", psSysData->apsLocalDevMemArena[0]);
	printk(KERN_INFO "   [1] => %p\n", psSysData->apsLocalDevMemArena[1]);
	printk(KERN_INFO "   [2] => %p\n", psSysData->apsLocalDevMemArena[2]);
	printk(KERN_INFO "   [3] => %p\n", psSysData->apsLocalDevMemArena[3]);
/*	printk(KERN_INFO "   name = %s\n", (char*)psSysData->apsLocalDevMemArena[0]);

	printk(KERN_INFO "   uQuantum = %lu\n", ra->uQuantum);
	printk(KERN_INFO "   pImportHandle = %p\n", ra->pImportHandle);
	printk(KERN_INFO "   pHeadSegment = %p\n", ra->pHeadSegment);
	printk(KERN_INFO "   pTailSegment = %p\n", ra->pTailSegment);
	printk(KERN_INFO "   pSegmentHash = %p\n", ra->pSegmentHash);
	printk(KERN_INFO "   pProcInfo = %p\n", ra->pProcInfo);
	printk(KERN_INFO "   pProcSegs = %p\n", ra->pProcSegs);
*/
	printk(KERN_INFO "  VersionString = \"%s\"\n", psSysData->pszVersionString);
	printk(KERN_INFO "  psGlobalEventObject = %p\n", psSysData->psGlobalEventObject);
	printk(KERN_INFO "  bFlushAll = %d\n", psSysData->bFlushAll);
}

//in mmu.c (no headers)
typedef struct _MMU_PT_INFO_
{
	IMG_VOID *hPTPageOSMemHandle;
	IMG_CPU_VIRTADDR PTPageCpuVAddr;
	IMG_UINT32 ui32ValidPTECount;
} MMU_PT_INFO;
#define SGX_MAX_PD_ENTRIES  (1<<(SGX_FEATURE_ADDRESS_SPACE_SIZE - SGX_MMU_PT_SHIFT - SGX_MMU_PAGE_SHIFT))

struct _MMU_CONTEXT_
{
	PVRSRV_DEVICE_NODE *psDeviceNode;
	IMG_CPU_VIRTADDR pvPDCpuVAddr;
	IMG_DEV_PHYADDR sPDDevPAddr;
	IMG_VOID *hPDOSMemHandle;
	MMU_PT_INFO *apsPTInfoList[SGX_MAX_PD_ENTRIES];
	PVRSRV_SGXDEV_INFO *psDevInfo;
#if defined(PDUMP)
	MMU_PT_INFOIMG_UINT32 ui32PDumpMMUContextID;
#endif
	struct _MMU_CONTEXT_ *psNext;
};

static void dump_structSizes(SGX_MISCINFO_STRUCT_SIZES *ssz) {
	if (!ssz) return;
#if defined (SGX_FEATURE_2D_HARDWARE)
	printk(KERN_INFO "      ui32Sizeof_2DCMD = %lx\n", ssz->ui32Sizeof_2DCMD);
	printk(KERN_INFO "      ui32Sizeof_2DCMD_SHARED = %lx\n", ssz->ui32Sizeof_2DCMD_SHARED);
#endif
	printk(KERN_INFO "      ui32Sizeof_CMDTA = %lx\n", ssz->ui32Sizeof_CMDTA);
	printk(KERN_INFO "      ui32Sizeof_CMDTA_SHARED = %lx\n", ssz->ui32Sizeof_CMDTA_SHARED);
	printk(KERN_INFO "      ui32Sizeof_TRANSFERCMD = %lx\n", ssz->ui32Sizeof_TRANSFERCMD);
	printk(KERN_INFO "      ui32Sizeof_TRANSFERCMD_SHARED = %lx\n", ssz->ui32Sizeof_TRANSFERCMD_SHARED);
	printk(KERN_INFO "      ui32Sizeof_3DREGISTERS = %lx\n", ssz->ui32Sizeof_3DREGISTERS);
	printk(KERN_INFO "      ui32Sizeof_HWPBDESC = %lx\n", ssz->ui32Sizeof_HWPBDESC);
	printk(KERN_INFO "      ui32Sizeof_HWRENDERCONTEXT = %lx\n", ssz->ui32Sizeof_HWRENDERCONTEXT);
	printk(KERN_INFO "      ui32Sizeof_HWRENDERDETAILS = %lx\n", ssz->ui32Sizeof_HWRENDERDETAILS);
	printk(KERN_INFO "      ui32Sizeof_HWRTDATA = %lx\n", ssz->ui32Sizeof_HWRTDATA);
	printk(KERN_INFO "      ui32Sizeof_HWRTDATASET = %lx\n", ssz->ui32Sizeof_HWRTDATASET);
	printk(KERN_INFO "      ui32Sizeof_HWTRANSFERCONTEXT = %lx\n", ssz->ui32Sizeof_HWTRANSFERCONTEXT);
	printk(KERN_INFO "      ui32Sizeof_HOST_CTL = %lx\n", ssz->ui32Sizeof_HOST_CTL);
	printk(KERN_INFO "      ui32Sizeof_COMMAND = %lx\n", ssz->ui32Sizeof_COMMAND);
}
static void dump_sgxInfos(PVRSRV_SGXDEV_INFO *sgx) {
	IMG_SYS_PHYADDR *pa;
	IMG_DEV_PHYADDR *dpa;
	if (!sgx || psSgxDevInfo == sgx) return;
	psSgxDevInfo = sgx;
	printk(KERN_INFO "     eDeviceType = %d\n", sgx->eDeviceType);
	printk(KERN_INFO "     eDeviceClass = %d\n", sgx->eDeviceClass);
	printk(KERN_INFO "     ui8VersionMajor = %u\n", sgx->ui8VersionMajor);
	printk(KERN_INFO "     ui8VersionMinor = %u\n", sgx->ui8VersionMinor);
	printk(KERN_INFO "     ui32CoreConfig = %lu\n", sgx->ui32CoreConfig);
	printk(KERN_INFO "     ui32CoreFlags = %lu\n", sgx->ui32CoreFlags);
	printk(KERN_INFO "     pvRegsBaseKM = %p\n", sgx->pvRegsBaseKM);
#if defined(SGX_FEATURE_HOST_PORT)
	printk(KERN_INFO "     pvHostPortBaseKM = %p\n", sgx->pvHostPortBaseKM);
	printk(KERN_INFO "     ui32HPSize = %lu\n", sgx->ui32HPSize);
	pa = &sgx->sHPSysPAddr; if (pa)
	printk(KERN_INFO "     sHPSysPAddr = %p\n", pa->uiAddr);
#endif
	printk(KERN_INFO "     hRegMapping = %p\n", sgx->hRegMapping);
	pa = &sgx->sRegsPhysBase; if (pa)
	printk(KERN_INFO "     sRegsPhysBase.uiAddr = %p\n", (void*) pa->uiAddr);
	printk(KERN_INFO "     ui32RegSize = 0x%x\n", (unsigned int) sgx->ui32RegSize);
#if defined(SUPPORT_EXTERNAL_SYSTEM_CACHE)
	printk(KERN_INFO "     ui32ExtSysCacheRegsSize = %lu\n", sgx->ui32ExtSysCacheRegsSize);
	//...
#endif
	printk(KERN_INFO "     ui32CoreClockSpeed = %lu\n", sgx->ui32CoreClockSpeed);
	printk(KERN_INFO "     ui32uKernelTimerClock = %lu\n", sgx->ui32uKernelTimerClock);
	printk(KERN_INFO "     psStubPBDescListKM = %p PVRSRV_STUB_PBDESC\n", sgx->psStubPBDescListKM);
	dpa = &sgx->sKernelPDDevPAddr; if (dpa)
	printk(KERN_INFO "     sKernelPDDevPAddr.uiAddr = %lx\n", dpa->uiAddr);
	printk(KERN_INFO "     pvDeviceMemoryHeap = %p\n", sgx->pvDeviceMemoryHeap);
	printk(KERN_INFO "     psKernelCCBMemInfo = %p\n", sgx->psKernelCCBMemInfo);
	printk(KERN_INFO "     psKernelCCB = %p\n", sgx->psKernelCCB);
	printk(KERN_INFO "     psKernelCCBInfo = %p\n", sgx->psKernelCCBInfo);
	printk(KERN_INFO "     psKernelCCBCtlMemInfo = %p\n", sgx->psKernelCCBCtlMemInfo);
	printk(KERN_INFO "     psKernelCCBCtl = %p\n", sgx->psKernelCCBCtl);
	printk(KERN_INFO "     psKernelCCBEventKickerMemInfo = %p\n", sgx->psKernelCCBEventKickerMemInfo);
	printk(KERN_INFO "     pui32KernelCCBEventKicker = %p\n", sgx->pui32KernelCCBEventKicker);
	printk(KERN_INFO "     psKernelSGXMiscMemInfo = %p\n", sgx->psKernelSGXMiscMemInfo);
	printk(KERN_INFO "     aui32HostKickAddr[] = 0x%x\n", (unsigned int) sgx->aui32HostKickAddr);
	printk(KERN_INFO "     ui32KickTACounter = %lu\n", sgx->ui32KickTACounter);
	printk(KERN_INFO "     ui32KickTARenderCounter = %lu\n", sgx->ui32KickTARenderCounter);
	//...
	printk(KERN_INFO "     ui32ClientRefCount = %lu\n", sgx->ui32ClientRefCount);
	printk(KERN_INFO "     ui32CacheControl = %lu\n", sgx->ui32CacheControl);
	printk(KERN_INFO "     ui32ClientBuildOptions = %lu\n", sgx->ui32ClientBuildOptions);
	printk(KERN_INFO "     sSGXStructSizes = %p SGX_MISCINFO_STRUCT_SIZES\n", (void*) &sgx->sSGXStructSizes);
	dump_structSizes(&sgx->sSGXStructSizes);
	printk(KERN_INFO "     pvMMUContextList = %p\n", sgx->pvMMUContextList);
	printk(KERN_INFO "     bForcePTOff = %d\n", sgx->bForcePTOff);
	printk(KERN_INFO "     ui32EDMTaskReg0 = 0x%lx\n", sgx->ui32EDMTaskReg0);
	printk(KERN_INFO "     ui32EDMTaskReg1 = 0x%lx\n", sgx->ui32EDMTaskReg1);
	printk(KERN_INFO "     ui32ClkGateStatusReg = 0x%lx\n", sgx->ui32ClkGateStatusReg);
	printk(KERN_INFO "     ui32ClkGateStatusMask = 0x%lx\n", sgx->ui32ClkGateStatusMask);
#if defined(SGX_FEATURE_MP)
	printk(KERN_INFO "     ui32MasterClkGateStatusReg = 0x%x\n", sgx->ui32MasterClkGateStatusReg);
	printk(KERN_INFO "     ui32MasterClkGateStatusMask = 0x%x\n", sgx->ui32MasterClkGateStatusMask);
#endif
	//printk(KERN_INFO "     sScripts = %p\n", sgx->sScripts); //SGX_INIT_SCRIPTS
	printk(KERN_INFO "     hBIFResetPDOSMemHandle = %p\n", sgx->hBIFResetPDOSMemHandle);
	dpa = &sgx->sBIFResetPDDevPAddr; if (dpa)
	printk(KERN_INFO "     sBIFResetPDDevPAddr.uiAddr = %lx\n", dpa->uiAddr);
	dpa = &sgx->sBIFResetPTDevPAddr; if (dpa)
	printk(KERN_INFO "     sBIFResetPTDevPAddr.uiAddr = %lx\n", dpa->uiAddr);
	dpa = &sgx->sBIFResetPageDevPAddr; if (dpa)
	printk(KERN_INFO "     sBIFResetPageDevPAddr.uiAddr = %lx\n", dpa->uiAddr);
	printk(KERN_INFO "     pui32BIFResetPD = %p\n", sgx->pui32BIFResetPD);
	printk(KERN_INFO "     pui32BIFResetPT = %p\n", sgx->pui32BIFResetPT);
#if defined(FIX_HW_BRN_22997) && defined(FIX_HW_BRN_23030) && defined(SGX_FEATURE_HOST_PORT)
#endif
#if defined(SUPPORT_HW_RECOVERY)
	printk(KERN_INFO "     hTimer = %p\n", sgx->hTimer);
	printk(KERN_INFO "     ui32TimeStamp = 0x%lx\n", sgx->ui32TimeStamp);
#endif
	printk(KERN_INFO "     ui32NumResets = %lu\n", sgx->ui32NumResets);
	printk(KERN_INFO "     psKernelSGXHostCtlMemInfo = %p\n", sgx->psKernelSGXHostCtlMemInfo);
	printk(KERN_INFO "     psSGXHostCtl = %p\n", sgx->psSGXHostCtl);
	printk(KERN_INFO "     psKernelSGXTA3DCtlMemInfo = %p\n", sgx->psKernelSGXTA3DCtlMemInfo);
	printk(KERN_INFO "     ui32Flags = %lu\n", sgx->ui32Flags);
	printk(KERN_INFO "     asSGXDevData[%d] = %p\n", SGX_MAX_DEV_DATA, sgx->asSGXDevData);
}
static void dump_MmuContext(MMU_CONTEXT *mmu) {
	IMG_DEV_PHYADDR *pa;
	if (!mmu) return;
	printk(KERN_INFO "    psDeviceNode = %p\n", mmu->psDeviceNode);
	printk(KERN_INFO "    pvPDCpuVAddr = %p\n", mmu->pvPDCpuVAddr);
	pa = &mmu->sPDDevPAddr; if (pa)
	printk(KERN_INFO "    sPDDevPAddr.uiAddr = %p\n", (void*) pa->uiAddr);
	printk(KERN_INFO "    hPDOSMemHandle = %p\n", mmu->hPDOSMemHandle);
	printk(KERN_INFO "    apsPTInfoList = %p\n", mmu->apsPTInfoList);
	printk(KERN_INFO "    psDevInfo => %p PVRSRV_SGXDEV_INFO\n", mmu->psDevInfo);
	dump_sgxInfos(mmu->psDevInfo);
#if defined(PDUMP)
	printk(KERN_INFO "    ui32PDumpMMUContextID = %p\n", mmu->ui32PDumpMMUContextID);
#endif
	printk(KERN_INFO "    psNext = %p\n", mmu->psNext);
}
static void dump_BufferManagerContext(BM_CONTEXT *buffer) {
	if (!buffer) return;
	printk(KERN_INFO "   psMMUContext => %p MMU_CONTEXT\n", buffer->psMMUContext);
	dump_MmuContext(buffer->psMMUContext);
	printk(KERN_INFO "   psBMHeap = %p\n", buffer->psBMHeap);
	printk(KERN_INFO "   psBMSharedHeap = %p\n", buffer->psBMSharedHeap);
	printk(KERN_INFO "   psDeviceNode = %p\n", buffer->psDeviceNode);
	printk(KERN_INFO "   pBufferHash = %p\n", buffer->pBufferHash);
	printk(KERN_INFO "   hResItem = %p\n", buffer->hResItem);
	printk(KERN_INFO "   ui32RefCount = %lu\n", buffer->ui32RefCount);
	printk(KERN_INFO "   psNext = %p\n", buffer->psNext);
	printk(KERN_INFO "   ppsThis = %p\n", buffer->ppsThis);
}
static void dump_DeviceNode(PVRSRV_DEVICE_NODE *node) {
	DEVICE_MEMORY_INFO *meminfo;
	//DEVICE_MEMORY_HEAP_INFO *psDeviceMemoryHeap;
	//psDeviceMemoryHeap = psDevMemoryInfo->psDeviceMemoryHeap;
	//printk(KERN_INFO " psDeviceMemoryHeap=%p\n", psDeviceMemoryHeap);

	printk(KERN_INFO " sDevId.eDeviceType = %d\n", node->sDevId.eDeviceType);
	printk(KERN_INFO " sDevId.eDeviceClass = %d\n", node->sDevId.eDeviceClass);
	printk(KERN_INFO " sDevId.ui32DeviceIndex = %lu\n", node->sDevId.ui32DeviceIndex);
	printk(KERN_INFO " ui32RefCount = %lu\n", node->ui32RefCount);
	printk(KERN_INFO " pvISRData = %p\n", node->pvISRData);
	printk(KERN_INFO " ui32SOCInterruptBit = %lu\n", node->ui32SOCInterruptBit);
	printk(KERN_INFO " bReProcessDeviceCommandComplete = %d\n", node->bReProcessDeviceCommandComplete);
	meminfo = &node->sDevMemoryInfo;
	printk(KERN_INFO " sDevMemoryInfo => %p DEVICE_MEMORY_INFO\n", meminfo);
	printk(KERN_INFO "  ui32AddressSpaceSizeLog2 = %lu\n", meminfo->ui32AddressSpaceSizeLog2);
	printk(KERN_INFO "  ui32Flags = %lu\n", meminfo->ui32Flags);
	printk(KERN_INFO "  ui32HeapCount = %lu\n", meminfo->ui32HeapCount);
	printk(KERN_INFO "  ui32SyncHeapID = %lu\n", meminfo->ui32SyncHeapID);
	printk(KERN_INFO "  ui32MappingHeapID = %lu\n", meminfo->ui32MappingHeapID);
	printk(KERN_INFO "  psDeviceMemoryHeap = %p\n", meminfo->psDeviceMemoryHeap);
	printk(KERN_INFO "  pBMKernelContext => %p BM_CONTEXT\n", meminfo->pBMKernelContext);
	dump_BufferManagerContext(meminfo->pBMKernelContext);
	printk(KERN_INFO "  pBMContext => %p BM_CONTEXT\n", meminfo->pBMContext);
	dump_BufferManagerContext(meminfo->pBMContext);
	printk(KERN_INFO " pvDevice = %p\n", node->pvDevice);
	printk(KERN_INFO " ui32pvDeviceSize = %lu\n", node->ui32pvDeviceSize);
	printk(KERN_INFO " hResManContext = %p\n", node->hResManContext);
	printk(KERN_INFO " psSysData => %p SYS_DATA\n", node->psSysData);
	if (!psSysData) {
		psSysData = node->psSysData;
		dump_SysData();
	}
	printk(KERN_INFO " psLocalDevMemArena = %p\n", node->psLocalDevMemArena);
	printk(KERN_INFO " ui32Flags = %lu\n", node->ui32Flags);
	printk(KERN_INFO " psNext = %p\n", node->psNext);
	printk(KERN_INFO " ppsThis = %p\n", node->ppsThis);
}

/* hooked function used when /proc/pvr/nodes is read */
static void ProcSeqShowSysNodes(struct seq_file *sfile, void* el) {
	PVRSRV_DEVICE_NODE *psDevNode = (PVRSRV_DEVICE_NODE*)el;
	if (el > PVR_PROC_SEQ_START_TOKEN) {
		printk(KERN_INFO "Entering ProcSeqShowSysNodes() sfile=%p el=%p PVRSRV_DEVICE_NODE\n", sfile, el);
		dump_DeviceNode(psDevNode);
	}
	HOOK_INVOKE(ProcSeqShowSysNodes, sfile, el);
	if (el > PVR_PROC_SEQ_START_TOKEN) {
		// last one, stop hooking
		if (psDevNode->psNext == NULL) {
			hook_exit();
			hooked = false;
		}
	}
}

struct hook_info g_hi[] = {
	HOOK_INIT(ProcSeqShowSysNodes), // when /proc/pvr/nodes is read (cat)
	HOOK_INIT_END
};

int __init init_ionpvr(void) {
	hook_init();
	hooked = true;
	return sniff_handle_base();
}
int release_sgx(void) {
	if (datainit) {
		SYMSEARCH_BIND_FUNCTION_TO(ionpvr, PVRSRVPerProcessDataDeInit, _PVRSRVPerProcessDataDeInit);
		_PVRSRVPerProcessDataDeInit();
	}
	if (connected) {
		SYMSEARCH_BIND_FUNCTION_TO(ionpvr, PVRSRVPerProcessDataDisconnect, _PVRSRVPerProcessDataDisconnect);
		_PVRSRVPerProcessDataDisconnect(KERNEL_ID);
	}
	return 0;
}
void __exit exit_ionpvr(void) {
	if (hooked) {
		hook_exit();
	}
	release_sgx();
}

module_init(init_ionpvr);
module_exit(exit_ionpvr);

MODULE_VERSION("0.2");
MODULE_DESCRIPTION("Ion test for Motorola 2.6.32 kernel");
MODULE_AUTHOR("Tanguy Pruvot");
MODULE_LICENSE("GPL");

