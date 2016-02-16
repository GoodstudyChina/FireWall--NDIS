/*++

Copyright (c) 1992-2000  Microsoft Corporation
 
Module Name:
 
    passthru.c

Abstract:

    Ndis Intermediate Miniport driver sample. This is a passthru driver.

Author:

Environment:


Revision History:


--*/


#include "precomp.h"
#include "common.h"
#pragma hdrstop
#pragma NDIS_INIT_FUNCTION(DriverEntry)

extern BOOLEAN UTIL_Initialize(ParamConfig *pParamConfig);
extern BOOLEAN UTIL_AddConfig(ParamConfig *pParamConfig);


NDIS_HANDLE         ProtHandle = NULL;
NDIS_HANDLE         DriverHandle = NULL;
NDIS_MEDIUM         MediumArray[4] =
                    {
                        NdisMedium802_3,    // Ethernet
                        NdisMedium802_5,    // Token-ring
                        NdisMediumFddi,     // Fddi
                        NdisMediumWan       // NDISWAN
                    };

NDIS_SPIN_LOCK     GlobalLock;

PADAPT             pAdaptList = NULL;
LONG               MiniportCount = 0;

NDIS_HANDLE        NdisWrapperHandle;

//
// To support ioctls from user-mode:
//

#define LINKNAME_STRING     L"\\DosDevices\\Passthru"
#define NTDEVICE_STRING     L"\\Device\\Passthru"


NDIS_HANDLE     NdisDeviceHandle = NULL;
PDEVICE_OBJECT  ControlDeviceObject = NULL;

enum _DEVICE_STATE
{
    PS_DEVICE_STATE_READY = 0,    // ready for create/delete
    PS_DEVICE_STATE_CREATING,    // create operation in progress
    PS_DEVICE_STATE_DELETING    // delete operation in progress
} ControlDeviceState = PS_DEVICE_STATE_READY;

// ����һ������ָ�����������Ndis���е�AddDeviceʵ��
AddDeviceFunc systemAddDevice = NULL; 

// ����4������ָ���������������ϵͳ�����IRP�Ĵ���
DispatchFunc systemCreate = NULL;
DispatchFunc systemWrite = NULL;
DispatchFunc systemRead = NULL;
DispatchFunc systemDeviceControl = NULL;

NTSTATUS myCreate(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp);
NTSTATUS myWrite(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp);
NTSTATUS myRead(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp);
NTSTATUS myDeviceControl(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp);
NTSTATUS myAddDevice(IN PDRIVER_OBJECT  DriverObject,
					 IN PDEVICE_OBJECT  PhysicalDeviceObject);

NTSTATUS
DriverEntry(
    IN PDRIVER_OBJECT        DriverObject,
    IN PUNICODE_STRING       RegistryPath
    )
/*++

Routine Description:

    First entry point to be called, when this driver is loaded.
    Register with NDIS as an intermediate driver.

Arguments:

    DriverObject - pointer to the system's driver object structure
        for this driver
    
    RegistryPath - system's registry path for this driver
    
Return Value:

    STATUS_SUCCESS if all initialization is successful, STATUS_XXX
    error code if not.

--*/
{
    NDIS_STATUS                        Status;
    NDIS_PROTOCOL_CHARACTERISTICS      PChars;
    NDIS_MINIPORT_CHARACTERISTICS      MChars;
    NDIS_STRING                        Name;

    Status = NDIS_STATUS_SUCCESS;
    NdisAllocateSpinLock(&GlobalLock);

    NdisMInitializeWrapper(&NdisWrapperHandle, DriverObject, RegistryPath, NULL);

    do
    {
        //
        // Register the miniport with NDIS. Note that it is the miniport
        // which was started as a driver and not the protocol. Also the miniport
        // must be registered prior to the protocol since the protocol's BindAdapter
        // handler can be initiated anytime and when it is, it must be ready to
        // start driver instances.
        //

        NdisZeroMemory(&MChars, sizeof(NDIS_MINIPORT_CHARACTERISTICS));

        MChars.MajorNdisVersion = PASSTHRU_MAJOR_NDIS_VERSION;
        MChars.MinorNdisVersion = PASSTHRU_MINOR_NDIS_VERSION;

        MChars.InitializeHandler = MPInitialize;
        MChars.QueryInformationHandler = MPQueryInformation;
        MChars.SetInformationHandler = MPSetInformation;
        MChars.ResetHandler = NULL;
        MChars.TransferDataHandler = MPTransferData;
        MChars.HaltHandler = MPHalt;
#ifdef NDIS51_MINIPORT
        MChars.CancelSendPacketsHandler = MPCancelSendPackets;
        MChars.PnPEventNotifyHandler = MPDevicePnPEvent;
        MChars.AdapterShutdownHandler = MPAdapterShutdown;
#endif // NDIS51_MINIPORT

        //
        // We will disable the check for hang timeout so we do not
        // need a check for hang handler!
        //
        MChars.CheckForHangHandler = NULL;
        MChars.ReturnPacketHandler = MPReturnPacket;

        //
        // Either the Send or the SendPackets handler should be specified.
        // If SendPackets handler is specified, SendHandler is ignored
        //
        MChars.SendHandler = NULL;    // MPSend;
        MChars.SendPacketsHandler = MPSendPackets;

        Status = NdisIMRegisterLayeredMiniport(NdisWrapperHandle,
                                                  &MChars,
                                                  sizeof(MChars),
                                                  &DriverHandle);
        if (Status != NDIS_STATUS_SUCCESS)
        {
            break;
        }

#ifndef WIN9X
        NdisMRegisterUnloadHandler(NdisWrapperHandle, PtUnload);
#endif

        //
        // Now register the protocol.
        //
        NdisZeroMemory(&PChars, sizeof(NDIS_PROTOCOL_CHARACTERISTICS));
        PChars.MajorNdisVersion = PASSTHRU_PROT_MAJOR_NDIS_VERSION;
        PChars.MinorNdisVersion = PASSTHRU_PROT_MINOR_NDIS_VERSION;

        //
        // Make sure the protocol-name matches the service-name
        // (from the INF) under which this protocol is installed.
        // This is needed to ensure that NDIS can correctly determine
        // the binding and call us to bind to miniports below.
        //
        NdisInitUnicodeString(&Name, L"Passthru");    // Protocol name
        PChars.Name = Name;
        PChars.OpenAdapterCompleteHandler = PtOpenAdapterComplete;
        PChars.CloseAdapterCompleteHandler = PtCloseAdapterComplete;
        PChars.SendCompleteHandler = PtSendComplete;
        PChars.TransferDataCompleteHandler = PtTransferDataComplete;
    
        PChars.ResetCompleteHandler = PtResetComplete;
        PChars.RequestCompleteHandler = PtRequestComplete;
        PChars.ReceiveHandler = PtReceive;
        PChars.ReceiveCompleteHandler = PtReceiveComplete;
        PChars.StatusHandler = PtStatus;
        PChars.StatusCompleteHandler = PtStatusComplete;
        PChars.BindAdapterHandler = PtBindAdapter;
        PChars.UnbindAdapterHandler = PtUnbindAdapter;
        PChars.UnloadHandler = PtUnloadProtocol;

        PChars.ReceivePacketHandler = PtReceivePacket;
        PChars.PnPEventHandler= PtPNPHandler;

        NdisRegisterProtocol(&Status,
                             &ProtHandle,
                             &PChars,
                             sizeof(NDIS_PROTOCOL_CHARACTERISTICS));

        if (Status != NDIS_STATUS_SUCCESS)
        {
            NdisIMDeregisterLayeredMiniport(DriverHandle);
            break;
        }

        NdisIMAssociateMiniport(DriverHandle, ProtHandle);
    }
    while (FALSE);

    if (Status != NDIS_STATUS_SUCCESS)
    {
        NdisTerminateWrapper(NdisWrapperHandle, NULL);
    }

	// ����ʵ��Hook
	// 
	systemAddDevice = DriverObject->DriverExtension->AddDevice; 
	DriverObject->DriverExtension->AddDevice = myAddDevice;

	// Hook�ַ�����
	//
	systemCreate = DriverObject->MajorFunction[IRP_MJ_CREATE];
	DriverObject->MajorFunction[IRP_MJ_CREATE] = myCreate;

	systemWrite = DriverObject->MajorFunction[IRP_MJ_WRITE];
	DriverObject->MajorFunction[IRP_MJ_WRITE] = myWrite;

	systemRead = DriverObject->MajorFunction[IRP_MJ_READ];
	DriverObject->MajorFunction[IRP_MJ_READ] = myRead;

	systemDeviceControl = DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL];
	DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = myDeviceControl;

    return(Status);
}


NDIS_STATUS
PtRegisterDevice(
    VOID
    )
/*++

Routine Description:

    Register an ioctl interface - a device object to be used for this
    purpose is created by NDIS when we call NdisMRegisterDevice.

    This routine is called whenever a new miniport instance is
    initialized. However, we only create one global device object,
    when the first miniport instance is initialized. This routine
    handles potential race conditions with PtDeregisterDevice via
    the ControlDeviceState and MiniportCount variables.

    NOTE: do not call this from DriverEntry; it will prevent the driver
    from being unloaded (e.g. on uninstall).// ��֪����Ϊʲô��Ҫ����һ��!! MV_COMM_3_17

Arguments:

    None

Return Value:

    NDIS_STATUS_SUCCESS if we successfully register a device object.

--*/
{
    NDIS_STATUS            Status = NDIS_STATUS_SUCCESS;
    UNICODE_STRING         DeviceName;
    UNICODE_STRING         DeviceLinkUnicodeString;
    PDRIVER_DISPATCH       DispatchTable[IRP_MJ_MAXIMUM_FUNCTION+1];

    DBGPRINT(("==>PtRegisterDevice\n"));

    NdisAcquireSpinLock(&GlobalLock);

    ++MiniportCount;
    
    if (1 == MiniportCount)
    {
        ASSERT(ControlDeviceState != PS_DEVICE_STATE_CREATING);

        //
        // Another thread could be running PtDeregisterDevice on
        // behalf of another miniport instance. If so, wait for
        // it to exit.
        //
        while (ControlDeviceState != PS_DEVICE_STATE_READY)
        {
            NdisReleaseSpinLock(&GlobalLock);
            NdisMSleep(1);
            NdisAcquireSpinLock(&GlobalLock);
        }

        ControlDeviceState = PS_DEVICE_STATE_CREATING;

        NdisReleaseSpinLock(&GlobalLock);

    
        NdisZeroMemory(DispatchTable, (IRP_MJ_MAXIMUM_FUNCTION+1) * sizeof(PDRIVER_DISPATCH));


//------------------------------added by Tanjon---------------------------------------

// BEGIN_PTUSERIO
		DispatchTable[IRP_MJ_CREATE] = DevOpen;
		DispatchTable[IRP_MJ_CLEANUP] = DevCleanup;
		DispatchTable[IRP_MJ_CLOSE] = DevClose;
		DispatchTable[IRP_MJ_DEVICE_CONTROL] = DevIoControl;
// END_PTUSERIO

//-----------------------------------------------------------------------------------

        NdisInitUnicodeString(&DeviceName, NTDEVICE_STRING);
        NdisInitUnicodeString(&DeviceLinkUnicodeString, LINKNAME_STRING);

        //
        // Create a device object and register our dispatch handlers
        //
        
        Status = NdisMRegisterDevice(
                    NdisWrapperHandle, 
                    &DeviceName,
                    &DeviceLinkUnicodeString,
                    &DispatchTable[0],
                    &ControlDeviceObject,
                    &NdisDeviceHandle
                    );

        NdisAcquireSpinLock(&GlobalLock);

        ControlDeviceState = PS_DEVICE_STATE_READY;
    }

    NdisReleaseSpinLock(&GlobalLock);

    DBGPRINT(("<==PtRegisterDevice: %x\n", Status));

    return (Status);
}


NTSTATUS
PtDispatch(
    IN PDEVICE_OBJECT    DeviceObject,
    IN PIRP              Irp
    )
/*++
Routine Description:

    Process IRPs sent to this device.

Arguments:

    DeviceObject - pointer to a device object
    Irp      - pointer to an I/O Request Packet

Return Value:

    NTSTATUS - STATUS_SUCCESS always - change this when adding
    real code to handle ioctls.

--*/
{
    PIO_STACK_LOCATION  irpStack;
    NTSTATUS            status = STATUS_SUCCESS;

    UNREFERENCED_PARAMETER(DeviceObject);
    
    DBGPRINT(("==>Pt Dispatch\n"));
    irpStack = IoGetCurrentIrpStackLocation(Irp);
      

    switch (irpStack->MajorFunction)
    {
        case IRP_MJ_CREATE:
            break;
            
        case IRP_MJ_CLEANUP:
            break;
            
        case IRP_MJ_CLOSE:
            break;        
            
        case IRP_MJ_DEVICE_CONTROL:
            //
            // Add code here to handle ioctl commands sent to passthru.
            //
            break;        
        default:
            break;
    }

    Irp->IoStatus.Status = status;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);

    DBGPRINT(("<== Pt Dispatch\n"));

    return status;

} 


NDIS_STATUS
PtDeregisterDevice(
    VOID
    )
/*++

Routine Description:

    Deregister the ioctl interface. This is called whenever a miniport
    instance is halted. When the last miniport instance is halted, we
    request NDIS to delete the device object

Arguments:

    NdisDeviceHandle - Handle returned by NdisMRegisterDevice

Return Value:

    NDIS_STATUS_SUCCESS if everything worked ok

--*/
{
    NDIS_STATUS Status = NDIS_STATUS_SUCCESS;

    DBGPRINT(("==>PassthruDeregisterDevice\n"));

    NdisAcquireSpinLock(&GlobalLock);

    ASSERT(MiniportCount > 0);

    --MiniportCount;
    
    if (0 == MiniportCount)
    {
        //
        // All miniport instances have been halted. Deregister
        // the control device.
        //

        ASSERT(ControlDeviceState == PS_DEVICE_STATE_READY);

        //
        // Block PtRegisterDevice() while we release the control
        // device lock and deregister the device.
        // 
        ControlDeviceState = PS_DEVICE_STATE_DELETING;

        NdisReleaseSpinLock(&GlobalLock);

        if (NdisDeviceHandle != NULL)
        {
            Status = NdisMDeregisterDevice(NdisDeviceHandle);
            NdisDeviceHandle = NULL;
        }

        NdisAcquireSpinLock(&GlobalLock);
        ControlDeviceState = PS_DEVICE_STATE_READY;
    }

    NdisReleaseSpinLock(&GlobalLock);

    DBGPRINT(("<== PassthruDeregisterDevice: %x\n", Status));
    return Status;
    
}

VOID
PtUnload(
    IN PDRIVER_OBJECT        DriverObject
    )
//
// PassThru driver unload function
//
{
    UNREFERENCED_PARAMETER(DriverObject);
    
    DBGPRINT(("PtUnload: entered\n"));
    
    PtUnloadProtocol();
    
    NdisIMDeregisterLayeredMiniport(DriverHandle);
    
    NdisFreeSpinLock(&GlobalLock);

    DBGPRINT(("PtUnload: done!\n"));
}

//------------------------------added by Tanjon---------------------------------------

NTSTATUS
DevOpen(
    IN PDEVICE_OBJECT    pDeviceObject,
    IN PIRP              pIrp
    )
{
    PIO_STACK_LOCATION  pIrpSp;
    NTSTATUS            NtStatus = STATUS_SUCCESS;
	
    UNREFERENCED_PARAMETER(pDeviceObject);
    
    pIrpSp = IoGetCurrentIrpStackLocation(pIrp);
	
    pIrpSp->FileObject->FsContext = NULL;
    pIrpSp->FileObject->FsContext2 = NULL;
	
    DBGPRINT(("==>Pt DevOpen: FileObject %p\n", pIrpSp->FileObject));
	
    pIrp->IoStatus.Information = 0;
    pIrp->IoStatus.Status = NtStatus;
	
    IoCompleteRequest(pIrp, IO_NO_INCREMENT);
	
    DBGPRINT(("<== Pt DevOpen\n"));
	
    return NtStatus;
} 
NTSTATUS
DevCleanup(
    IN PDEVICE_OBJECT    pDeviceObject,
    IN PIRP              pIrp
    )
{
    PIO_STACK_LOCATION  pIrpSp;
    NTSTATUS            NtStatus = STATUS_SUCCESS;
	
    UNREFERENCED_PARAMETER(pDeviceObject);
    
    pIrpSp = IoGetCurrentIrpStackLocation(pIrp);
	
    DBGPRINT(("==>Pt DevCleanup: FileObject %p\n", pIrpSp->FileObject ));
	
    pIrp->IoStatus.Information = 0;
    pIrp->IoStatus.Status = NtStatus;
    IoCompleteRequest(pIrp, IO_NO_INCREMENT);
	
    DBGPRINT(("<== Pt DevCleanup\n"));
	
    return NtStatus;
} 
NTSTATUS
DevClose(
    IN PDEVICE_OBJECT    pDeviceObject,
    IN PIRP              pIrp
    )
{
    PIO_STACK_LOCATION  pIrpSp;
    NTSTATUS            NtStatus = STATUS_SUCCESS;
	
    UNREFERENCED_PARAMETER(pDeviceObject);
    
    pIrpSp = IoGetCurrentIrpStackLocation(pIrp);
	
    DBGPRINT(("==>Pt DevClose: FileObject %p\n", pIrpSp->FileObject ));
	
    pIrpSp->FileObject->FsContext = NULL;
    pIrpSp->FileObject->FsContext2 = NULL;
	
    pIrp->IoStatus.Information = 0;
    pIrp->IoStatus.Status = NtStatus;
    IoCompleteRequest(pIrp, IO_NO_INCREMENT);
	
    DBGPRINT(("<== Pt DevClose\n"));
	
    return NtStatus;
} 
NTSTATUS
DevIoControl(
    IN PDEVICE_OBJECT    pDeviceObject,
    IN PIRP              pIrp
    )
{
    PIO_STACK_LOCATION  pIrpSp;
    NTSTATUS            NtStatus = STATUS_SUCCESS;
    ULONG               BytesReturned = 0;
    ULONG               FunctionCode;
    PUCHAR              ioBuffer = NULL;
    ULONG               inputBufferLength;
    ULONG               outputBufferLength;
    
    UNREFERENCED_PARAMETER(pDeviceObject);
    
    pIrpSp = IoGetCurrentIrpStackLocation(pIrp);
    
    ioBuffer = pIrp->AssociatedIrp.SystemBuffer;
    inputBufferLength  = pIrpSp->Parameters.DeviceIoControl.InputBufferLength;
    outputBufferLength = pIrpSp->Parameters.DeviceIoControl.OutputBufferLength;
    
    FunctionCode = pIrpSp->Parameters.DeviceIoControl.IoControlCode;
    
    DBGPRINT(("==>Pt DevIoControl: FileObject %p\n", pIrpSp->FileObject ));
	

    
    switch (FunctionCode)
    {
//----------------------------------added by Tanjon-----------------------------------
	
		case IOCTL_PTUSERIO_ADDCONFIG:
			DbgPrint(("==>AddConfig!\n"));
			if(inputBufferLength == sizeof(ParamConfig))
			{
				if(UTIL_AddConfig((struct ParamConfig*)ioBuffer))
					pIrp->IoStatus.Status = STATUS_SUCCESS;
				DbgPrint("<==AddConfig!\n");
			}
			else 
				pIrp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
				pIrp->IoStatus.Information = 0L;
	break;
//-------------------------------------------------------------------------------------     
		default:
            NtStatus = STATUS_NOT_SUPPORTED;
   break;
    }
    
    if (NtStatus != STATUS_PENDING)
    {
        pIrp->IoStatus.Information = BytesReturned;
        pIrp->IoStatus.Status = NtStatus;
        IoCompleteRequest(pIrp, IO_NO_INCREMENT);
    }
    
    DBGPRINT(("<== Pt DevIoControl\n"));
    
    return NtStatus;
}

//-------------------------------------------------------------------------------