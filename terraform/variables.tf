variable "project_id" {
  description = "GCP project ID to deploy into."
  type        = string
}

variable "region" {
  description = "GCP region for the cluster and Artifact Registry."
  type        = string
  default     = "us-central1"
}

variable "cluster_name" {
  description = "Name of the GKE cluster."
  type        = string
  default     = "grpc-lb-cluster"
}

variable "node_count" {
  description = "Nodes per zone in the primary node pool."
  type        = number
  default     = 1

  validation {
    condition     = var.node_count >= 1 && var.node_count <= 10
    error_message = "node_count must be between 1 and 10; this project needs 1-2."
  }
}

variable "machine_type" {
  description = "Machine type for cluster nodes. e2-standard-2 is the smallest that comfortably runs the system pods plus this workload."
  type        = string
  default     = "e2-standard-2"
}

variable "preemptible_nodes" {
  description = "Use preemptible (Spot) nodes. Cheap, and they get reclaimed with 30s notice -- which is a realistic way to watch the LB evict a backend that vanishes for reasons nobody chose."
  type        = bool
  default     = true
}

variable "subnet_cidr" {
  description = "Primary CIDR for the node subnet."
  type        = string
  default     = "10.10.0.0/20"
}

variable "pods_cidr" {
  description = "Secondary CIDR for pod IPs (VPC-native cluster)."
  type        = string
  default     = "10.20.0.0/16"
}

variable "services_cidr" {
  description = "Secondary CIDR for Service ClusterIPs."
  type        = string
  default     = "10.30.0.0/20"
}

variable "authorized_networks" {
  description = "CIDRs allowed to reach the cluster control plane. Defaults to none, which means kubectl works only from inside the VPC or via an authorized network you add explicitly. Do not put 0.0.0.0/0 here."
  type = list(object({
    cidr_block   = string
    display_name = string
  }))
  default = []
}
