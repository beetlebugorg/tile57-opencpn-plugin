// Print per-pmtiles bounds + zoom range as CSV: path,minz,maxz,west,south,east,north
#include "tile57.h"
#include <stdio.h>
int main(int c, char** v) {
    for (int i = 1; i < c; ++i) {
        tile57_chart* ch = tile57_chart_open_pmtiles(v[i]);
        if (!ch) { fprintf(stderr, "openfail %s\n", v[i]); continue; }
        tile57_chart_info in;
        tile57_chart_get_info(ch, &in);
        if (in.has_bounds)
            printf("%s,%d,%d,%.6f,%.6f,%.6f,%.6f\n", v[i], in.min_zoom, in.max_zoom,
                   in.west, in.south, in.east, in.north);
        tile57_chart_close(ch);
    }
    return 0;
}
