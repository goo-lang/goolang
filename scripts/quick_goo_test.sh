#!/bin/bash

# Quick test of our Goo programs

echo "🚀 Testing Goo Code Examples"
echo "============================"

echo ""
echo "📁 Available Goo programs:"
echo ""

for file in *.goo; do
    if [ -f "$file" ]; then
        lines=$(wc -l < "$file")
        echo "  📄 $file ($lines lines)"
        echo "     $(head -1 "$file" | sed 's|^// ||')"
    fi
done

echo ""
echo "🎨 Showing syntax-highlighted preview of hello_world.goo:"
echo "========================================================="

# Use our syntax highlighting to show a preview
cat hello_world.goo

echo ""
echo "📝 Example Goo Language Features:"
echo "================================="

echo ""
echo "1. 🚫 Nullable Types:"
echo "   var user ?User = findUser(id)"
echo "   if user? {"
echo "       fmt.Printf(\"Found: %s\", user!.name)"
echo "   }"

echo ""
echo "2. ❗ Error Unions:"
echo "   func divide(a, b int) (!int, !Error) {"
echo "       if b == 0 {"
echo "           return nil, Error(\"division by zero\")"
echo "       }"
echo "       return a / b, nil"
echo "   }"

echo ""
echo "3. 📡 Channels & Concurrency:"
echo "   ch := make(chan int, 10)"
echo "   go func() {"
echo "       ch <- 42"
echo "   }()"
echo "   value := <-ch"

echo ""
echo "4. 🎯 Pattern Matching:"
echo "   switch result {"
echo "   case Ok(value):"
echo "       return value"
echo "   case Err(error):"
echo "       return handleError(error)"
echo "   }"

echo ""
echo "✨ To explore further:"
echo "   • Run: ./bin/goo-repl-enhanced (interactive REPL)"
echo "   • Check: examples/ directory for more samples"
echo "   • View: docs/features/ for documentation"