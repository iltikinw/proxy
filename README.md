# tinyproxy.c - A tiny concurrent web proxy with caching written in C.
Written as part of CMU's Computer Systems Course. Received a 100% grade.
## Info on web proxies
A web proxy acts as an intermediary between client web browsers and server web servers providing web content. When a browser uses a proxy, it contacts the proxy instead of the server; the proxy forwards requests and responses between client and server.
## How my implementation works
My implementation uses the main function to continuously accept client connections, and serves those connections via the serve function. I use threads to allow for the proxy to serve clients concurrently. Additionally, I cache server responses in a LRU cache implemented with a circular doubly-linked list. More cache details can be found below.
### High-level overview:
1. Client connection request accepted; served in peer thread.
2. Request line parsed.
3. If request response exists in cache, served directly to client.
4. If not, connect to server, write response, serve back to client.
5. Server response then saved in the cache.
## Info on caches used
A software cache functions as key-value storage; saves some block of data with its associated key such that future requests of the key return the stored data.
## How my cache implementation works
My implementation uses a circular doubly-linked list of cache blocks.
Key implementation details:
* Request URIs used as keys.
* Server response text used as values.
* Block replacement via a least-recently-used policy (LRU).
* Cache automatically resizes by removing LRU block whenever necessary; stays under ```MAX_CACHE_SIZE``` in size.
* Synchronization via mutexes and block reference counts.
## Demos
The version publicly available in this repository does not work on its own. For demos, please contact me at iltikinw@gmail.com, and I'd love to connect!