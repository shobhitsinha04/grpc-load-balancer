# GKE cluster for the gRPC load balancer.
#
# NOT APPLIED: Terraform is not installed on the development machine and no GCP
# project was billed for this. `terraform plan` has never run against it. It is
# written to be reviewed, and it is honest about that. See README.md.

# ---------------------------------------------------------------------------
# Network
#
# VPC-native (alias IP) rather than routes-based: pods get real VPC IPs from a
# secondary range, which is required for a private cluster and is the default
# GKE steers toward. auto_create_subnetworks is off so the ranges below are the
# only ones that exist.
# ---------------------------------------------------------------------------
resource "google_compute_network" "vpc" {
  name                    = "${var.cluster_name}-vpc"
  auto_create_subnetworks = false
}

resource "google_compute_subnetwork" "subnet" {
  name          = "${var.cluster_name}-subnet"
  region        = var.region
  network       = google_compute_network.vpc.id
  ip_cidr_range = var.subnet_cidr

  # Private Google Access lets nodes without external IPs pull images from
  # Artifact Registry. Without it, a private cluster cannot start its own pods.
  private_ip_google_access = true

  secondary_ip_range {
    range_name    = "pods"
    ip_cidr_range = var.pods_cidr
  }

  secondary_ip_range {
    range_name    = "services"
    ip_cidr_range = var.services_cidr
  }
}

# Nodes have no external IPs, so egress (apt, image pulls from outside AR) has
# to go through NAT.
resource "google_compute_router" "router" {
  name    = "${var.cluster_name}-router"
  region  = var.region
  network = google_compute_network.vpc.id
}

resource "google_compute_router_nat" "nat" {
  name                               = "${var.cluster_name}-nat"
  router                             = google_compute_router.router.name
  region                             = var.region
  nat_ip_allocate_option             = "AUTO_ONLY"
  source_subnetwork_ip_ranges_to_nat = "ALL_SUBNETWORKS_ALL_IP_RANGES"
}

# ---------------------------------------------------------------------------
# Artifact Registry
# ---------------------------------------------------------------------------
resource "google_artifact_registry_repository" "images" {
  location      = var.region
  repository_id = "grpc-lb"
  format        = "DOCKER"
  description   = "Images for the L7 gRPC load balancer and echo backends."
}

# ---------------------------------------------------------------------------
# Cluster
# ---------------------------------------------------------------------------
resource "google_container_cluster" "primary" {
  name     = var.cluster_name
  location = var.region

  # GKE insists on a node pool at creation; this immediately removes it so the
  # managed pool below is the only one. Standard practice -- it keeps node
  # config in a resource Terraform can actually update in place.
  remove_default_node_pool = true
  initial_node_count       = 1

  network    = google_compute_network.vpc.id
  subnetwork = google_compute_subnetwork.subnet.id

  networking_mode = "VPC_NATIVE"
  ip_allocation_policy {
    cluster_secondary_range_name  = "pods"
    services_secondary_range_name = "services"
  }

  private_cluster_config {
    enable_private_nodes    = true
    enable_private_endpoint = false # Keep kubectl reachable from outside the VPC.
    master_ipv4_cidr_block  = "172.16.0.0/28"
  }

  master_authorized_networks_config {
    dynamic "cidr_blocks" {
      for_each = var.authorized_networks
      content {
        cidr_block   = cidr_blocks.value.cidr_block
        display_name = cidr_blocks.value.display_name
      }
    }
  }

  # Lets pods authenticate as GCP service accounts without mounted key files.
  # Not needed by this workload today; enabled because retrofitting it means
  # recreating the cluster.
  workload_identity_config {
    workload_pool = "${var.project_id}.svc.id.goog"
  }

  # Managed Prometheus scrapes the prometheus.io/* pod annotations set in
  # k8s/20-lb.yaml, so lb_* metrics land in Cloud Monitoring with no Prometheus
  # deployment of our own. The self-hosted stack in docker-compose.yml is the
  # local equivalent.
  monitoring_config {
    managed_prometheus {
      enabled = true
    }
    enable_components = ["SYSTEM_COMPONENTS"]
  }

  logging_config {
    enable_components = ["SYSTEM_COMPONENTS", "WORKLOADS"]
  }

  release_channel {
    channel = "REGULAR"
  }

  # Deletion protection defaults to true and would make `terraform destroy`
  # fail. This is a demo cluster that should be trivially destroyable -- an
  # abandoned GKE cluster is an expensive thing to forget about.
  deletion_protection = false
}

resource "google_container_node_pool" "primary" {
  name     = "${var.cluster_name}-pool"
  location = var.region
  cluster  = google_container_cluster.primary.name

  # Per zone. A regional cluster spans 3 zones, so node_count = 1 yields 3
  # nodes -- which is what gives the backend StatefulSet somewhere to spread.
  node_count = var.node_count

  node_config {
    machine_type = var.machine_type
    disk_size_gb = 50
    disk_type    = "pd-standard"
    spot         = var.preemptible_nodes

    service_account = google_service_account.nodes.email
    oauth_scopes    = ["https://www.googleapis.com/auth/cloud-platform"]

    workload_metadata_config {
      mode = "GKE_METADATA"
    }

    shielded_instance_config {
      enable_secure_boot          = true
      enable_integrity_monitoring = true
    }

    labels = {
      workload = "grpc-lb"
    }
  }

  management {
    auto_repair  = true
    auto_upgrade = true
  }

  upgrade_settings {
    max_surge       = 1
    max_unavailable = 0
  }
}

# Dedicated node identity. The default compute service account is over-permissioned
# (Editor on the whole project); this one gets only what nodes actually need:
# pull images, write logs and metrics.
resource "google_service_account" "nodes" {
  account_id   = "${var.cluster_name}-nodes"
  display_name = "GKE nodes for ${var.cluster_name}"
}

resource "google_project_iam_member" "nodes" {
  for_each = toset([
    "roles/artifactregistry.reader",
    "roles/logging.logWriter",
    "roles/monitoring.metricWriter",
    "roles/monitoring.viewer",
    "roles/stackdriver.resourceMetadata.writer",
  ])

  project = var.project_id
  role    = each.value
  member  = "serviceAccount:${google_service_account.nodes.email}"
}
