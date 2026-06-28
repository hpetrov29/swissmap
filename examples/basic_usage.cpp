#include <swissmap/fixed_map.hpp>

#include <iostream>

int main()
{
    // Maps an order ID to its position in the order book.
    swiss::swissmap<int, int> order_index(16);

    order_index.insert(1001, 0);
    order_index.insert(1002, 1);

    if (const int *index = order_index.find(1002))
    {
        std::cout << "Order 1002 is at index " << *index << '\n';
    }

    if (!order_index.contains(9999))
    {
        std::cout << "Order 9999 was not found\n";
    }
}
