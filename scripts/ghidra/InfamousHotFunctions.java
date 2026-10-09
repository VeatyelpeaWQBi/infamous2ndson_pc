// Export a bounded neighborhood of measured hotspots; never modify the input binary.
// @category inFAMOUS
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.nio.file.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

public class InfamousHotFunctions extends GhidraScript {
    @Override public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length != 1) throw new IllegalArgumentException("Output directory required");
        Path out = Path.of(args[0]); Files.createDirectories(out);
        Address target = toAddr(0x3f3ed0);
        // The extracted ELF may be imported at a default image base by the loader.
        if (!currentProgram.getMemory().contains(target) ||
            currentProgram.getMemory().getByte(target) != (byte)0x55) {
            throw new IllegalStateException("Hotspot address/prologue mismatch; check image base");
        }
        LinkedHashSet<Function> selected = new LinkedHashSet<>();
        for (long value : new long[]{0x3f3ed0,0x3fdff0,0x16dd50,0x612110}) {
            Address address = toAddr(value);
            Function function = getFunctionAt(address);
            if (function == null) { disassemble(address); function = createFunction(address, null); }
            if (function != null) selected.add(function);
        }
        Function root = getFunctionAt(target);
        if (root == null) throw new IllegalStateException("Root function not recovered");
        for (Reference ref : getReferencesTo(target)) {
            Function caller = getFunctionContaining(ref.getFromAddress());
            if (caller != null && selected.size() < 32) selected.add(caller);
        }
        for (Function function : new ArrayList<>(selected)) {
            for (Function callee : function.getCalledFunctions(monitor)) {
                if (selected.size() < 40 && !callee.isExternal()) selected.add(callee);
            }
        }
        DecompInterface decompiler = new DecompInterface();
        try {
            if (!decompiler.openProgram(currentProgram))
                throw new IllegalStateException(decompiler.getLastMessage());
            StringBuilder index = new StringBuilder("address\tname\tbytes\tdecompiled\tcallers\tcallees\n");
            for (Function function : selected) {
                monitor.checkCancelled();
                DecompileResults result = decompiler.decompileFunction(function, 30, monitor);
                String address = function.getEntryPoint().toString();
                boolean success = result.decompileCompleted() && result.getDecompiledFunction()!=null;
                String text = success ? result.getDecompiledFunction().getC() : "/* FAILED: " + result.getErrorMessage() + " */\n";
                Files.writeString(out.resolve(address + ".c"), text, StandardCharsets.UTF_8);
                StringBuilder asm = new StringBuilder();
                InstructionIterator instructions = currentProgram.getListing().getInstructions(function.getBody(),true);
                while(instructions.hasNext()) { Instruction instruction=instructions.next(); asm.append(instruction.getAddress()).append(" ").append(instruction).append('\n'); }
                Files.writeString(out.resolve(address + ".asm"),asm,StandardCharsets.UTF_8);
                index.append(address).append('\t').append(function.getName()).append('\t').append(function.getBody().getNumAddresses()).append('\t').append(success).append('\t');
                for(Function caller:function.getCallingFunctions(monitor)) index.append(caller.getEntryPoint()).append(',');
                index.append('\t');
                for(Function callee:function.getCalledFunctions(monitor)) index.append(callee.getEntryPoint()).append(',');
                index.append('\n');
                println("HOT_FUNCTION " + address + " decompiled=" + success);
            }
            Files.writeString(out.resolve("functions.tsv"),index,StandardCharsets.UTF_8);
            Files.writeString(out.resolve("program.txt"),"language="+currentProgram.getLanguageID()+"\nimage_base="+currentProgram.getImageBase()+"\nfunctions="+currentProgram.getFunctionManager().getFunctionCount()+"\n",StandardCharsets.UTF_8);
        } finally { decompiler.dispose(); }
    }
}
