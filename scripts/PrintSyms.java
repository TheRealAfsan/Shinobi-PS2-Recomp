import ghidra.app.script.GhidraScript;
import ghidra.program.model.symbol.*;
import java.io.*;

public class PrintSyms extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] wanted = {"__nj_storebuffer_", "__nj_storebuffer_max_", "__nj_storebufer_dma_ch_", "_nj_storebuffer_current_",
                           "__nj_punchthrough_mode_", "_hySystemBufferKick", "njSendDisplayList", "KfUpdateScreen"};
        try (PrintWriter w = new PrintWriter(new FileWriter("X:\\HAPPYNESS\\shinobi_port\\syms.txt"))) {
            SymbolTable st = currentProgram.getSymbolTable();
            for (String n : wanted) {
                for (Symbol s : st.getSymbols(n)) {
                    w.println(n + " " + s.getAddress() + " " + s.getSymbolType());
                }
            }
            // anything containing "gameSystem" or "TaskSys" (case-insensitive)
            SymbolIterator it = st.getAllSymbols(true);
            int n = 0;
            while (it.hasNext() && n < 40) {
                Symbol s = it.next();
                String nm = s.getName();
                if (nm.toLowerCase().contains("storebuf") && s.getSymbolType() != SymbolType.FUNCTION) {
                    w.println("~ " + nm + " " + s.getAddress());
                    n++;
                }
            }
        }
    }
}
