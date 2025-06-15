# 🏗️ Goo Microservice Template

A production-ready microservice template with observability, health checks, and graceful shutdown.

## ✨ Features

- **Health Checks**: Comprehensive dependency monitoring
- **Metrics & Monitoring**: Prometheus metrics integration
- **Distributed Tracing**: Jaeger integration for request tracing
- **Graceful Shutdown**: Clean resource cleanup on termination
- **Error Unions**: Safe error handling throughout the service
- **Request Middleware**: Automatic request tracking and metrics
- **CORS Support**: Cross-origin resource sharing
- **Database Integration**: PostgreSQL with connection pooling

## 🚀 Quick Start

```bash
# Create new microservice from template
goo new microservice my-service

# Set environment variables
export DATABASE_URL="postgres://user:pass@localhost/mydb"
export METRICS_URL="http://localhost:9090"
export JAEGER_URL="http://localhost:14268/api/traces"

# Build and run
cd my-service
goo build
./my-service
```

## 📊 Observability

### Health Check Endpoint
```http
GET /health
```

**Response:**
```json
{
  "success": true,
  "data": {
    "status": "healthy",
    "timestamp": "2024-01-15T10:30:00Z",
    "services": {
      "database": true,
      "cache": true
    },
    "uptime_seconds": 3600
  },
  "request_id": "req_1642248600000000000"
}
```

### Metrics Endpoint
```http
GET /metrics
```

Returns Prometheus-formatted metrics:
```
# HELP requests_total Total number of HTTP requests
# TYPE requests_total counter
requests_total 1234

# HELP request_duration_seconds Request duration in seconds
# TYPE request_duration_seconds histogram
request_duration_seconds_bucket{le="0.1"} 100
```

## 🏗️ Architecture Features

### Dependency Injection
```goo
type Service struct {
    db      *sql.DB
    metrics *metrics.Client
    tracer  *tracing.Tracer
}

func NewService() (!*Service, !Error) {
    // Initialize all dependencies with error handling
    db, dbErr := sql.Open("postgres", os.Getenv("DATABASE_URL"))
    if dbErr! {
        return nil, Error("Database connection failed")
    }
    // ... more initialization
}
```

### Request Middleware
```goo
func (s *Service) requestMiddleware(next http.HandlerFunc) http.HandlerFunc {
    return func(w http.ResponseWriter, r *http.Request) {
        // Add request ID, tracing, metrics
        requestID := generateRequestID()
        span := s.tracer.StartSpan(r.URL.Path)
        
        // Call next handler
        next(w, r)
        
        // Record metrics and cleanup
        s.metrics.Timer("request_duration").Record(duration)
    }
}
```

### Error Union Pattern
```goo
func (s *Service) healthCheck(ctx context.Context) (!HealthStatus, !Error) {
    dbErr := s.db.PingContext(ctx)
    if dbErr! {
        return nil, Error("Database health check failed")
    }
    
    return HealthStatus{Status: "healthy"}, nil
}
```

## 🔧 Configuration

### Environment Variables
```bash
# Database
DATABASE_URL=postgres://user:pass@localhost:5432/mydb

# Metrics & Observability  
METRICS_URL=http://prometheus:9090
JAEGER_URL=http://jaeger:14268/api/traces

# Service Configuration
PORT=8080
LOG_LEVEL=info
SERVICE_NAME=my-microservice
```

### Docker Support
```dockerfile
FROM goo:latest as builder
WORKDIR /app
COPY . .
RUN goo build -o service

FROM alpine:latest
RUN apk --no-cache add ca-certificates
WORKDIR /root/
COPY --from=builder /app/service .
EXPOSE 8080
CMD ["./service"]
```

## 📈 Monitoring Stack

### Prometheus Configuration
```yaml
scrape_configs:
  - job_name: 'my-microservice'
    static_configs:
      - targets: ['localhost:8080']
    metrics_path: '/metrics'
    scrape_interval: 15s
```

### Grafana Dashboards
- Request rate and latency
- Error rate monitoring
- Health check status
- Database connection metrics

## 🧪 Testing

### Unit Tests
```goo
func TestHealthCheck(t *testing.T) {
    service := NewTestService()
    
    health, err := service.healthCheck(context.Background())
    if err! {
        t.Fatalf("Health check failed: %s", err!)
    }
    
    if health!.Status != "healthy" {
        t.Errorf("Expected healthy status, got %s", health!.Status)
    }
}
```

### Integration Tests
```bash
# Test health endpoint
curl http://localhost:8080/health

# Test metrics endpoint
curl http://localhost:8080/metrics
```

## 🔄 Deployment

### Kubernetes
```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: my-microservice
spec:
  replicas: 3
  selector:
    matchLabels:
      app: my-microservice
  template:
    spec:
      containers:
      - name: service
        image: my-microservice:latest
        ports:
        - containerPort: 8080
        livenessProbe:
          httpGet:
            path: /health
            port: 8080
          initialDelaySeconds: 30
          periodSeconds: 10
```

### Load Balancer Configuration
```yaml
apiVersion: v1
kind: Service
metadata:
  name: my-microservice-service
spec:
  selector:
    app: my-microservice
  ports:
  - port: 80
    targetPort: 8080
  type: LoadBalancer
```

## 🔐 Security Best Practices

1. **Input Validation**: Use Goo's contract programming
2. **Authentication**: JWT middleware integration
3. **Rate Limiting**: Request throttling middleware
4. **HTTPS**: TLS configuration
5. **Secrets Management**: Environment variable encryption

## 📚 Key Benefits

- **Type Safety**: Compile-time guarantees prevent runtime errors
- **Memory Safety**: No null pointer dereferences or buffer overflows
- **Error Handling**: Explicit error handling with error unions
- **Observability**: Built-in metrics, tracing, and health checks
- **Performance**: Zero-cost abstractions with LLVM optimization
- **Scalability**: Designed for horizontal scaling and load balancing