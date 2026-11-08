import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import java.io.*;

public class ExportDecomp extends GhidraScript {
    @Override
    public void run() throws Exception {
        String out = "X:\\HAPPYNESS\\shinobi_ghidra\\export";
        new File(out).mkdirs();
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        try (PrintWriter list = new PrintWriter(new FileWriter(out + "\\functions.txt"));
             PrintWriter c = new PrintWriter(new FileWriter(out + "\\shinobi_decompiled.c"))) {
            list.println("address\tsize\tname");
            int n = 0, fail = 0;
            for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
                list.println(f.getEntryPoint() + "\t" + f.getBody().getNumAddresses() + "\t" + f.getName());
                DecompileResults r = di.decompileFunction(f, 60, monitor);
                if (r.decompileCompleted()) {
                    c.println("// " + f.getName() + " @ " + f.getEntryPoint());
                    c.println(r.getDecompiledFunction().getC());
                } else {
                    fail++;
                    c.println("// FAILED: " + f.getName() + " @ " + f.getEntryPoint());
                }
                n++;
            }
            println("Exported " + n + " functions, " + fail + " failed");
        }
    }
}
