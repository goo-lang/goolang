#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// Simple test program for debugging
int global_var = 42;

int add_numbers(int a, int b) {
    int sum = a + b;
    printf("Adding %d + %d = %d\n", a, b, sum);
    return sum;
}

int factorial(int n) {
    if (n <= 1) {
        return 1;
    }
    return n * factorial(n - 1);
}

void print_array(int* arr, int size) {
    printf("Array contents: ");
    for (int i = 0; i < size; i++) {
        printf("%d ", arr[i]);
    }
    printf("\n");
}

int main(int argc, char* argv[]) {
    printf("Test program starting...\n");
    
    // Local variables
    int local_var = 10;
    int result;
    
    // Function call
    result = add_numbers(local_var, global_var);
    printf("Result: %d\n", result);
    
    // Array operations
    int numbers[] = {1, 2, 3, 4, 5};
    print_array(numbers, 5);
    
    // Recursive function
    int fact = factorial(5);
    printf("Factorial of 5: %d\n", fact);
    
    // Loop
    printf("Counting down: ");
    for (int i = 5; i > 0; i--) {
        printf("%d ", i);
        sleep(1);  // Give time for debugging
    }
    printf("\n");
    
    // Conditional
    if (result > 50) {
        printf("Result is large\n");
    } else {
        printf("Result is small\n");
    }
    
    printf("Test program finished\n");
    return 0;
}