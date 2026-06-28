# swissmap

A small C++20 Swiss-table-style hash map with fixed capacity and selectable
array-of-structures (AoS) or structure-of-arrays (SoA) bucket storage.

The map allocates its storage during construction and never rehashes, making
its allocation behavior predictable after startup. Insertions return `false`
when a key already exists or the map has reached its maximum load. The map is intended for
single-threaded use and does not provide internal synchronization.

## Usage

```cpp
#include <swissmap/fixed_map.hpp>

swiss::swissmap<int, int> order_index(1024);

order_index.insert(1001, 42);

if (const int* index = order_index.find(1001))
{
    // *index == 42
}
```

Capacity must be a power of two and at least 16. The maximum load is 7/8 of
the requested capacity.

The default layout is selected automatically based on slot padding. A layout
can also be requested explicitly:

```cpp
using aos_map = swiss::swissmap_aos<int, int>;
using soa_map = swiss::swissmap_soa<int, int>;
```
