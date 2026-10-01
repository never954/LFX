#include <iostream>
#include <vector>

int filterSum(const std::vector<int>& nums, int limit) {
    int total = 0;
    for (int n : nums) {
        if (n < limit) {
            total += n * 3;
        } else {
            limit += 1;
        }
    }
    return total;
}

int main() {
    std::cout << filterSum({4, 1, 5, 6, 2, 8, 3}, 5) << '\n';
    return 0;
}
