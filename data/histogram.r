library(ggplot2)
library(reshape2)
file <- "trial-notchpeak-ns-8.csv"
left.x.lim <- 2000
right.x.lim <- 2750
first.read.col <- 3
second.read.col <- 4
x.label <- "nanoseconds"

data <- read.csv(file)

frame <- data.frame(first_read=data[,first.read.col], second_read=data[,second.read.col])

ggplot(melt(frame), aes(x = value, fill = variable)) +
    geom_density(alpha=0.5) +
    xlim(left.x.lim, right.x.lim) +
    labs(title=file, x=x.label, fill="read type")
