SimpleSvmHook
==============

Introduction
-------------

SimpleSvmHook is a research purpose hypervisor for Windows on AMD processors.
It hooks kernel mode functions and protects them from being detected using
Nested Page Tables (NPT), part of AMD Virtualization (AMD-V) technology.

This project is meant to serve as an example implementation of virtual machine
introspection (VMI) on AMD processors and highlight differences from similar
VMI implementations on Intel processors.

If you already know about [DdiMon](https://github.com/tandasat/DdiMon), this is
an AMD counterpart of it from the functionality perspective except few things
described below.


Overview of Stealth Hook Implementation
----------------------------------------
A stealth hook is a type of hook that is not visible from the outside of the
monitor or inspector component. In the context of VMI, stealth hook is installed
and managed by a hypervisor into guest code to redirect execution of specified
addresses while being not easily detectable by the guest.

One of common ways to implement stealth hook within a hypervisor is to split
“view” of memory for read/write and execute access from the guest using Second
Level Address Translation (SLAT), namely, Extended Page Table (EPT) on Intel
and Nested Page Tables (NPT) for AMD processors.

SLAT introduces one more address translation step, that is, translation from the
guest physical address (GPA) to the system physical address (SPA). This
essentially allows a hypervisor to set up a mapping of a virtual address in the
guest and a backing physical memory address. The below diagram illustrates how
SLAT can be configured and address translation will result in.

    GPA              SPA     Memory Contents
    -----------------------------------------
    0x1000 –(SLAT)-> 0xa000  ...
    0x2000 –(SLAT)-> 0xb000  48 89 54 24 10
    ...

SLAT can also configure permission of the GPA against the guest; for instance,
GPA:0x2000 can be configured as readable/writable but not executable. When the
guest attempts to access a GPA in a way not permitted by SLAT, the processor
triggers VM-exit so that a hypervisor can take necessary actions, such as
updating the permission or inject #GP into the guest.

Stealth hook is often implemented by leveraging those capabilities. Take DdiMon
as an example, when the hypervisor installs stealth hook, it creates a copy of
the target page, sets 0xCC into the address to hook, then sets up EPT to make
the page execute-only (0xb000 in the below example).

    GPA                 SPA     Memory Contents
    --------------------------------------------
    0x2000 –(EPT --X)-> 0xb000  CC 89 54 24 10
                        0xf000  48 89 54 24 10  (The copied page. Unused yet)
    ...

When the guest attempts to execute the address, the hypervisor:
  1. traps #BP
  2. changes the instruction pointer of the guest to our handler function
  3. lets the guest run

and when the guest attempts to read from or write to the address, the hypervisor:
  1. traps VM-exit caused due to access violation
  2. updates EPT to associate the address with the copied page, which does *not*
     contain 0xCC, with the readable/writable permission

    GPA                 SPA     Memory Contents
    --------------------------------------------
    0x2000 –(EPT RW-)\  0xb000  CC 89 54 24 10
                      ->0xf000  48 89 54 24 10  (The copied page)
    ...

  3. single steps and lets the guest complete the read or write operation
  4. updates EPT to revert the settings

    GPA                 SPA     Memory Contents
    --------------------------------------------
    0x2000 –(EPT --X)-> 0xb000  CC 89 54 24 10
                        0xf000  48 89 54 24 10  (The copied page)
    ...

  5. lets the guest run

Those operations allow the hypervisor to redirect execution of the guest while
keeping the hook invisible from the guest. Also, notice that EPT configurations
are reverted to the original state, and the next execute or read/write access
can be handled in the same way.

However, this cannot be implemented directly on AMD processors.


Implementation on AMD Processors and Limitations
-------------------------------------------------

The previously described technique cannot be implemented on AMD processors due
to lack the execute-only permission.

NPT, the AMD implementation of SLAT, does not permit a hypervisor to configure
the permission as execute-only because there is no bit to indicate whether the
page is readable. To make the page executable, it must also be readable at the
same time. This limitation requires significant changes in a way to hide hooks.

Firstly, a hypervisor needs to set the unmodified readable/writable page as the
default association of the target page  (0xb000 in the below example).

    GPA                 SPA     Memory Contents
    --------------------------------------------
    0x2000 –(NPT RW-)-> 0xb000  48 89 54 24 10
                        0xf000  CC 89 54 24 10  (The copied page. Unused yet)
    ...

This introduces additional performance cost due to more frequent VM-exit (recall
that the address being hooked is code and much more likely accessed for
execution).

Secondly, the hook has to remain visible in some situation. Let us see why.

When the guest attempts to execute the address, the hypervisor:
  1. traps VM-exit caused due to access violation
  2. updates NPT to associate the address with the copied page, which contains
     0xCC, with the readable/writable/executable permission.

    GPA                 SPA     Memory Contents
    --------------------------------------------
    0x2000 –(NPT RWX)\  0xb000  48 89 54 24 10
                      ->0xf000  CC 89 54 24 10  (The copied page)
    ...

  3. lets the guest run
  4. traps #BP
  5. changes the instruction pointer of the guest to our handler function

Notice that while hook was executed, the hooked page remains readable with 0xCC.

This is a critical issue yet resolution is challenging because:
  - the page cannot be non-readable while being executable because of the
    limitation of NPT,
  - the page contents cannot be reverted to the original contents since the
    guest may execute the hooked address while 0xCC is removed, and
  - AMD-V lacks the Monitor Trap Flag equivalent feature that could have been
    used to overcome the second point.

The partial solution to this issue is to trap the guest when it attempts to
execute the outside of the hooked page, and then, hide 0xCC. This can be
achieved by setting all pages but the hooked page non-executable, then, when the
guest jumps out to the outside of the hooked page, the hypervisor:
  1. traps VM-exit caused due to access violation
  2. updates NPT to make all pages but the hooked page executable again
  3. updates NPT to associate the hooked page with the unmodified page with the
     readable/writable permission

    GPA                 SPA     Memory Contents
    --------------------------------------------
    0x2000 –(NPT RW-)-> 0xb000  48 89 54 24 10
                        0xf000  CC 89 54 24 10  (The copied page)
    ...

  4. lets the guest run

This way, the hook remains invisible while the guest is executing any code
outside of the hooked page. In other words, the guest can still see the hook
while it is executing the same page as the hooked page. This may be a
significant limitation if hook must be installed on code that may perform a
self-integrity check, but it is unlikely a real issue if hook is installed
against known kernel code such as NTOSKRNL and Win32k.

The other downside of this design is overhead of NTP manipulation. VM-exit, when
the guest jumps into and out from the hooked page, has non-negligible
performance penalty because the hypervisor must update multiple NPT entries to
change the executable permission of all pages.

Delay in Windows boot time measured by Windows Performance Analyzer with the
current implementation was 20% on HP [EliteBook 725 G4](http://www8.hp.com/ca/en/products/laptops/product-detail.html?oid=11084766#!tab=models)
(AMD A12 PRO-8830B), while it was only 9% with DdiMon on [Dell Latitude E6410](http://www.dell.com/us/en/business/notebooks/latitude-e6410/pd.aspx?refid=latitude-e6410&cs=04&s=bsd)
(Intel Core i7-620M). Given that the tested AMD processor is almost the 7 years
newer model than the tested Intel processor and expected to have much more
optimized implementation of AMD-V than that of VT-x on the old Intel processor,
this high number (20%) is likely because of the heavyweight VM-exit handling.

While this overhead did not appear to be noticeable on normal load, it could be
the real issue depending on load, and possibly, the number of hooks installed.


Conclusion
-----------

This project demonstrated that implementation of stealth hook on AMD processors
was possible with the caveat that hook had to remain visible from code in the
same page and that performance overhead introduced by the explained design was
considerably higher than that of similar implementation on Intel processors.

Those limitation can be ignored in some use cases such as instrumentation of
known kernel API for research, but could prevent researchers from developing
highly-stealth VMI tools and tools for performance sensitive environments.


Installation and Uninstallation
--------------------------------

To build SimpleSvmHook from source code, clone full source code from GitHub with
the below command and compile it on a supported version of Visual Studio.

    $ git clone https://github.com/tandasat/SimpleSvmHook.git

You have to enable test signing to install the driver before installing it. To
do that, open the command prompt with the administrator privilege and run the
following command, and then restart the system to activate the change:

    >bcdedit /set testsigning on

To install and uninstall the SimpleSvmHook driver, use the `sc` command. For
installation and start:

    >sc create SimpleSvmHook type= kernel binPath= C:\Users\user\Desktop\SimpleSvmHook.sys
    >sc start SimpleSvmHook

Note that the driver may fail to start due to failure to disassemble code for
hooking. SimpleSvmHook can only handle known byte patterns (instructions) and
does not attempt to disassemble unknown byte patterns. You can resolve such
errors by adding new patterns to the "FindFirstInstruction" function and
recompiling the driver.

For uninstallation:

    >sc stop SimpleSvmHook
    >sc delete SimpleSvmHook
    >bcdedit /deletevalue testsigning


Output
-------

All debug output are saved in `C:\Windows\SimpleSvmHook.log`.

This screenshot shows example output from installed hooks as well as that those
hooks are not visible from the system (the local kernel debugger).

![HookInstalled](/Images/HookInstalled.png)


Supported Platforms
--------------------

- Windows 10 x64 and Windows 7 x64
- AMD Processors with SVM and NPT support
- Visual Studio 15.7.5 or later for compilation

Note that emulation of NTP in VMware is significantly slow. To try out
SimpleSvmHook on VMware, set `SIMPLESVMHOOK_SINGLE_HOOK` to 1 and recompile the
driver.


Resources
----------

- SimpleSvm
  - https://github.com/tandasat/SimpleSvm

- DdiMon
  - https://github.com/tandasat/DdiMon
