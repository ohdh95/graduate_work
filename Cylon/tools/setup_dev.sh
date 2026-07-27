cxl create-region -m -t ram  -d decoder0.0 -w 1 -g 4096 mem0

# Touch all the pages to trigger cold EPT faults and populate the EPT tables.
daxctl reconfigure-device --mode=devdax --force dax0.0
./femu-cxl-test/cxl_warmup

if [ $1 == "system_ram" ]; then
    echo "Reconfiguring DAX device to system RAM mode..."
    # will be appear as a CPU-less NUMA Node in the system.
    daxctl reconfigure-device --mode=system-ram --force dax0.0
elif [ $1 == "devdax" ]; then
    echo "Configured DAX device to devdax mode..."
else
    echo "Usage: $0 [system_ram|devdax]"
fi