#pragma once

// Moho's std::sort, the 2005 MSVC library's introsort (faf-re
// legacy/algorithms/Sort.h): equal elements come out in its own order.

#include <cstddef>
#include <utility>

namespace osc::moho_sort {

namespace detail {

inline constexpr std::ptrdiff_t kInsertionSortMax = 32;
inline constexpr std::ptrdiff_t kNintherCountLimit = 40;

template <class T> void swap_values(T& a, T& b) {
    T temp = a;
    a = b;
    b = temp;
}

template <class T, class Compare> void median3(T* a, T* b, T* c, Compare comp) {
    if (comp(*b, *a)) {
        swap_values(*a, *b);
    }
    if (comp(*c, *b)) {
        swap_values(*b, *c);
    }
    if (comp(*b, *a)) {
        swap_values(*a, *b);
    }
}

template <class T, class Compare> void ninther(T* first, T* middle, T* last, Compare comp) {
    const std::ptrdiff_t step = (last - first) / 8;
    T* const last_in = last - 1;
    median3(first, first + step, first + 2 * step, comp);
    median3(middle - step, middle, middle + step, comp);
    median3(last_in - 2 * step, last_in - step, last_in, comp);
    median3(first + step, middle, last_in - step, comp);
}

template <class T> void rotate(T* first, T* middle, T* last) {
    if (first == middle || middle == last) {
        return;
    }
    const std::ptrdiff_t shift = middle - first;
    std::ptrdiff_t count = last - first;
    for (std::ptrdiff_t factor = shift; factor != 0;) {
        const std::ptrdiff_t next = count % factor;
        count = factor;
        factor = next;
    }
    if (count < last - first) {
        for (; count > 0; --count) {
            T* const hole = first + count;
            T* next = hole;
            T value = *hole;
            T* next1 = next + shift == last ? first : next + shift;
            while (next1 != hole) {
                *next = *next1;
                next = next1;
                next1 = shift < last - next1 ? next1 + shift : first + (shift - (last - next1));
            }
            *next = value;
        }
    }
}

template <class T, class Compare> void insertion_sort(T* first, T* last, Compare comp) {
    if (first == last) {
        return;
    }
    for (T* next = first; ++next != last;) {
        T* next1 = next;
        if (comp(*next, *first)) {
            rotate(first, next, ++next1);
        } else {
            for (T* first1 = next1; comp(*next, *--first1); next1 = first1) {}
            rotate(next1, next, next + 1);
        }
    }
}

template <class T, class Compare>
void adjust_heap(T* first, std::ptrdiff_t hole, std::ptrdiff_t count, T value, Compare comp) {
    const std::ptrdiff_t top = hole;
    std::ptrdiff_t child = 2 * hole + 2;
    for (; child < count; child = 2 * child + 2) {
        if (comp(first[child], first[child - 1])) {
            --child;
        }
        first[hole] = first[child];
        hole = child;
    }
    if (child == count) {
        first[hole] = first[count - 1];
        hole = count - 1;
    }
    for (std::ptrdiff_t parent = (hole - 1) / 2; top < hole && comp(first[parent], value);
         parent = (hole - 1) / 2) {
        first[hole] = first[parent];
        hole = parent;
    }
    first[hole] = value;
}

template <class T, class Compare> void heap_sort(T* first, T* last, Compare comp) {
    const std::ptrdiff_t n = last - first;
    if (n >= 2) {
        for (std::ptrdiff_t hole = n / 2; hole > 0;) {
            --hole;
            adjust_heap(first, hole, n, first[hole], comp);
        }
    }
    for (std::ptrdiff_t count = n; count > 1; --count) {
        T value = first[count - 1];
        first[count - 1] = first[0];
        adjust_heap(first, 0, count - 1, value, comp);
    }
}

template <class T, class Compare> std::pair<T*, T*> partition(T* first, T* last, Compare comp) {
    T* const middle = first + (last - first) / 2;
    if (last - 1 - first > kNintherCountLimit) {
        ninther(first, middle, last, comp);
    } else {
        median3(first, middle, last - 1, comp);
    }
    const T pivot = *middle;
    T* pfirst = middle;
    T* plast = pfirst + 1;
    while (first < pfirst && !comp(*(pfirst - 1), pivot) && !comp(pivot, *(pfirst - 1))) {
        --pfirst;
    }
    while (plast < last && !comp(*plast, pivot) && !comp(pivot, *plast)) {
        ++plast;
    }
    T* gfirst = plast;
    T* glast = pfirst;
    for (;;) {
        for (; gfirst < last; ++gfirst) {
            if (comp(pivot, *gfirst)) {
                continue;
            }
            if (comp(*gfirst, pivot)) {
                break;
            }
            if (plast++ != gfirst) {
                swap_values(*(plast - 1), *gfirst);
            }
        }
        for (; first < glast; --glast) {
            if (comp(*(glast - 1), pivot)) {
                continue;
            }
            if (comp(pivot, *(glast - 1))) {
                break;
            }
            if (--pfirst != glast - 1) {
                swap_values(*pfirst, *(glast - 1));
            }
        }
        if (glast == first && gfirst == last) {
            return {pfirst, plast};
        }
        if (glast == first) {
            if (plast != gfirst) {
                swap_values(*pfirst, *plast);
            }
            ++plast;
            swap_values(*pfirst, *gfirst);
            ++pfirst;
            ++gfirst;
        } else if (gfirst == last) {
            --glast;
            --pfirst;
            if (glast != pfirst) {
                swap_values(*glast, *pfirst);
            }
            --plast;
            swap_values(*pfirst, *plast);
        } else {
            --glast;
            swap_values(*gfirst, *glast);
            ++gfirst;
        }
    }
}

template <class T, class Compare> void sort(T* first, T* last, std::ptrdiff_t ideal, Compare comp) {
    while (kInsertionSortMax < last - first && ideal > 0) {
        const std::pair<T*, T*> mid = partition(first, last, comp);
        ideal = ideal / 2 + ideal / 2 / 2;
        if (mid.first - first < last - mid.second) {
            sort(first, mid.first, ideal, comp);
            first = mid.second;
        } else {
            sort(mid.second, last, ideal, comp);
            last = mid.first;
        }
    }
    if (kInsertionSortMax < last - first) {
        heap_sort(first, last, comp);
    } else if (last - first > 1) {
        insertion_sort(first, last, comp);
    }
}

} // namespace detail

template <class T, class Compare> void sort(T* first, T* last, Compare comp) {
    detail::sort(first, last, last - first, comp);
}

} // namespace osc::moho_sort
