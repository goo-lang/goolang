# 🌐 Goo Web API Template

A production-ready web API template demonstrating Goo's safety features for backend development.

## ✨ Features

- **Error Unions**: Safe error handling without exceptions
- **Nullable Types**: Null safety for optional data
- **Type-Safe JSON**: Automatic serialization with struct tags
- **Graceful Error Responses**: Consistent API error format
- **Health Check Endpoint**: Built-in monitoring endpoint

## 🚀 Quick Start

```bash
# Create new project from template
goo new web-api my-api

# Build and run
cd my-api
goo build
./my-api
```

## 📡 API Endpoints

### Get User
```http
GET /api/user?id=1
```

**Success Response:**
```json
{
  "success": true,
  "data": {
    "id": 1,
    "name": "User 1",
    "email": "user1@example.com"
  }
}
```

**Error Response:**
```json
{
  "success": false,
  "error": "Invalid user ID"
}
```

### Health Check
```http
GET /health
```

**Response:**
```json
{
  "success": true,
  "data": "OK"
}
```

## 🔧 Customization

1. **Add Database**: Replace mock data with real database calls
2. **Add Authentication**: Implement JWT or session-based auth
3. **Add Validation**: Use Goo's contract programming for input validation
4. **Add Middleware**: Implement logging, CORS, rate limiting
5. **Add Tests**: Use Goo's built-in testing framework

## 📚 Key Concepts Demonstrated

- **Error Unions (`!T`)**: Functions return either success value or error
- **Nullable Types (`?T`)**: Optional fields in responses
- **Pattern Matching**: Safe unwrapping with `if err!` syntax
- **Type Safety**: Compile-time guarantees for JSON serialization
- **Memory Safety**: No null pointer dereferences possible

## 🔄 Next Steps

1. Add database integration
2. Implement authentication middleware
3. Add comprehensive error handling
4. Set up logging and monitoring
5. Deploy with Docker/Kubernetes