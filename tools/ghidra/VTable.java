// Args: <outDir> <hexAddrInsideVtable>. Walks back/forward over consecutive function pointers,
// prints each slot (index, offset, function) and decompiles every slot into outDir/<program>/vt_<addr>/
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import java.io.*;
public class VTable extends GhidraScript {
    Function fnAt(Address a) throws Exception {
        long v = getInt(a) & 0xffffffffL;
        Address t = toAddr(v);
        return currentProgram.getMemory().contains(t) ? getFunctionAt(t) : null;
    }
    @Override public void run() throws Exception {
        String[] a = getScriptArgs();
        Address p = toAddr(a[1]);
        Address start = p;
        while (fnAt(start.subtract(4)) != null && getReferencesTo(start).length == 0) start = start.subtract(4);
        File out = new File(a[0], currentProgram.getName() + "/vt_" + start); out.mkdirs();
        StringBuilder refs = new StringBuilder();
        for (Reference r : getReferencesTo(start)) { Function f = getFunctionContaining(r.getFromAddress()); refs.append(f == null ? r.getFromAddress().toString() : f.getName()).append(' '); }
        println("OUT vtable " + start + " referenced by: " + refs);
        DecompInterface dec = new DecompInterface(); dec.openProgram(currentProgram);
        Address q = start; int i = 0;
        while (true) {
            Function f = fnAt(q);
            if (f == null || (i > 0 && getReferencesTo(q).length > 0)) break;
            DecompileResults res = dec.decompileFunction(f, 60, monitor);
            String c = res.decompileCompleted() ? res.getDecompiledFunction().getC() : "";
            println(String.format("OUT slot %2d +0x%02x %s lines=%d", i, i * 4, f.getName(), c.split("\n").length));
            try (PrintWriter w = new PrintWriter(new File(out, String.format("%02d_%s.c", i, f.getName())))) { w.print(c); }
            q = q.add(4); i++;
        }
        dec.dispose();
    }
}
